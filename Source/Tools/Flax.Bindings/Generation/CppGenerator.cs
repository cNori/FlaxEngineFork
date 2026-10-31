using System.Security.Cryptography;
using System.Text;
using VETools.Text;

namespace VETools.Generation;

internal enum NativeEntryPointNaming { Readable, Hashed, Protected }

internal sealed class CppGenerationResult
{
    public string Source = "";
    public string InputHeader = "";
    public List<LogEntry> Diagnostics = [];
    public bool Success => !Diagnostics.Any(entry => entry.BlocksGeneration);
    public Dictionary<EmittedBinding, string> EntryPoints = [];
    public Dictionary<EmittedBinding, string> WrapperMethods = [];
    public Dictionary<EmittedBinding, string> Upcasts = [];
    public Dictionary<EmittedBinding, string> Releases = [];
    public Dictionary<EmittedBinding, string> Events = [];
}

internal static class CppGenerator
{
    sealed class WrapperScope(string name, bool isClass)
    {
        public string Name = name;
        public bool IsClass = isClass;
        public Dictionary<string, WrapperScope> Children = new(StringComparer.Ordinal);
        public List<string> Methods = [];
        public WrapperScope Child(string name, bool isClass)
        {
            if (!Children.TryGetValue(name, out var child)) Children.Add(name, child = new(name, isClass));
            if (child.IsClass != isClass) throw new NotSupportedException("Wrapper namespace/type collision: " + name);
            return child;
        }
        public void Emit(StringBuilder code, int depth = 0)
        {
            string pad = new(' ', depth * 4);
            code.AppendLine(pad + (IsClass ? "class " : "namespace ") + Name);
            code.AppendLine(pad + "{");
            if (IsClass) code.AppendLine(pad + "public:");
            foreach (var child in Children.Values) child.Emit(code, depth + 1);
            foreach (string method in Methods)
                foreach (string line in method.Split('\n')) if (line.Length != 0) code.AppendLine(pad + "    " + line.TrimEnd('\r'));
            code.AppendLine(pad + (IsClass ? "};" : "}"));
        }
    }

    // Native forwarding backend. No export filtering or source-tree walk here.
    public static CppGenerationResult Generate(CSharpGenerationResult managed, string headerPath, NativeEntryPointNaming naming = NativeEntryPointNaming.Readable, string? engineVersion = null)
    {
        if (!Enum.IsDefined(naming)) throw new ArgumentOutOfRangeException(nameof(naming));
        bool protectedBindings = naming == NativeEntryPointNaming.Protected;
        if (protectedBindings && string.IsNullOrWhiteSpace(engineVersion))
            throw new ArgumentException("Protected bindings require an explicit engine version.", nameof(engineVersion));
        var entries = NativeBindingPass.Prepare(managed);
        var ownedClasses = entries.Where(e => e.NativeFunction?.IsConstructor == true && e.Native.Parent is ClassDescriptor).Select(e => (ClassDescriptor)e.Native.Parent!).ToHashSet();
        WrapperScope root = new("CppGlue", false);
        Dictionary<EmittedBinding, (WrapperScope Scope, string Qualified)> routes = [];
        Dictionary<string, (WrapperScope Scope, string Qualified)> globalRoutes = new(StringComparer.Ordinal);
        // Reserve namespace names before choosing synthetic global-call class names.
        foreach (var entry in entries.Where(e => e.Kind == EmittedBindingKind.Function))
        {
            var scope = root;
            foreach (string part in string.Concat(entry.Native.NamespaceTokens.Select(t => t.text)).Split("::", StringSplitOptions.RemoveEmptyEntries))
                scope = scope.Child(part, false);
        }
        // Create class scopes first, so the synthetic global-call class cannot shadow one.
        foreach (var entry in entries.Where(e => e.Kind == EmittedBindingKind.Type && (e.ClassBase != null || e.Native is ClassDescriptor c && ownedClasses.Contains(c)))) Route(entry, false);
        foreach (var entry in entries.Where(e => e.Kind == EmittedBindingKind.Function && e.Native.Parent is TypeDescriptor)) Route(entry, false);
        foreach (var entry in entries.Where(e => e.Kind == EmittedBindingKind.Function && e.Native.Parent is not TypeDescriptor)) Route(entry, true);
        void Route(EmittedBinding entry, bool global)
        {
            var scope = root;
            List<string> path = ["CppGlue"];
            foreach (string part in string.Concat(entry.Native.NamespaceTokens.Select(t => t.text)).Split("::", StringSplitOptions.RemoveEmptyEntries))
            { scope = scope.Child(part, false); path.Add(part); }
            Stack<string> classes = new();
            for (var parent = entry.Native is TypeDescriptor ? entry.Native : entry.Native.Parent; parent is TypeDescriptor; parent = parent.Parent) classes.Push(parent.Name.text);
            foreach (string part in classes) { scope = scope.Child(part, true); path.Add(part); }
            if (global)
            {
                string globals = "GlobalCalls";
                while (scope.Children.ContainsKey(globals)) globals += "_";
                // Reuse one synthetic class for all free functions in this namespace.
                string namespacePath = string.Join("::", path);
                if (globalRoutes.TryGetValue(namespacePath, out var existing)) { routes.Add(entry, existing); return; }
                scope = scope.Child(globals, true); path.Add(globals);
                var route = (scope, "::" + string.Join("::", path));
                globalRoutes.Add(namespacePath, route); routes.Add(entry, route); return;
            }
            routes.Add(entry, (scope, "::" + string.Join("::", path)));
        }
        var readableCounts = entries.Where(e => e.Kind == EmittedBindingKind.Function)
            .GroupBy(e => ReadableName(e.Native.QualifiedName)).ToDictionary(g => g.Key, g => g.Count());
        HashSet<string> usedNames = new(StringComparer.Ordinal);
        CppGenerationResult result = new();
        var codecs = entries.SelectMany(e => new[] { e.NativeType, e.NativeFunction?.ReturnType }.Concat(e.NativeFunction?.Parameters ?? []).Where(t => t != null).SelectMany(t => Converters(t!))).Concat(EventInterop.CopiedConverters(entries)).Distinct().ToArray();
        var requirements = NativeSupportRequirements.Merge(codecs.Select(c => c.Bodies?.NativeRequirements ?? new()).Concat(EventInterop.Requirements(entries)).Prepend(new() {
            Includes = codecs.Any(c => c.Bodies != null) ? ["<memory>", "<type_traits>", "<cstddef>", "<cstdint>", "<stdexcept>", "<utility>"] : ["<memory>", "<type_traits>", "<cstddef>", "<cstdint>", "<stdexcept>"]
        }));
        StringBuilder code = new();
        StringBuilder bridges = new();
        string header = Path.GetFullPath(headerPath).Replace('\\', '/');
        result.InputHeader = header;
        if (header.Contains('"') || header.Contains('\n') || header.Contains('\r'))
            throw new ArgumentException("Invalid native header path.", nameof(headerPath));
        code.AppendLine("// <auto-generated />\n// Status: 0 success, 1 null or misaligned argument, 2 native exception.");
        code.AppendLine("// Constructor results transfer ownership; other pointer/reference results are borrowed.");
        code.AppendLine($"#include \"{header}\"");
        foreach (string injected in managed.NativeInjectedCode.Distinct(StringComparer.Ordinal)) code.AppendLine(injected);
        // Typedef ownership can differ from the template definition's header.
        // Accessors/receivers require the complete native definition, not merely
        // the forward declaration present in the requested input header.
        var requiredHeaders = entries.SelectMany(entry => new[] { entry.Native as TypeDescriptor, entry.Native.Parent as TypeDescriptor, entry.ClassBase })
            .OfType<TypeDescriptor>().Select(type => type.ConcreteTemplate?.Definition.SourceFile?.Path ?? type.SourceFile?.Path)
            .OfType<string>().Concat(entries.SelectMany(entry => new[] { entry.NativeType, entry.NativeFunction?.ReturnType }.Concat(entry.NativeFunction?.Parameters ?? []).OfType<TypeInfo>().SelectMany(AutomaticFieldConverters.Converters).SelectMany(c => c.DefinitionHeaders)))
            .Concat(entries.SelectMany(e => e.Parameters ?? []).Select(p => p.InputPointerArray?.Element.SourceFile?.Path).OfType<string>())
            .Concat(entries.SelectMany(HandleNETcpp.DefinitionHeaders))
            .Concat(entries.Where(entry => entry.Kind == EmittedBindingKind.Event).SelectMany(EventInterop.DefinitionHeaders))
            .Concat(entries.SelectMany(entry => new[] { entry.NativeType, entry.NativeFunction?.ReturnType }.Concat(entry.NativeFunction?.Parameters ?? []).OfType<TypeInfo>().SelectMany(ConverterDefinitionHeaders)))
            .Where(System.IO.File.Exists).Select(path => Path.GetFullPath(path).Replace('\\', '/'))
            .Where(path => !string.Equals(path, header, StringComparison.OrdinalIgnoreCase)).Distinct(StringComparer.OrdinalIgnoreCase).Order(StringComparer.OrdinalIgnoreCase);
        foreach (string required in requiredHeaders)
        {
            if (required.Contains('"') || required.Contains('\n') || required.Contains('\r')) throw new ArgumentException("Invalid native dependency header path.");
            code.AppendLine($"#include \"{required}\"");
        }
        foreach (string include in requirements.Includes.Concat(entries.Any(e => e.Parameters?.Any(p => p.InputPointerArray != null) == true) ? new[] { "<vector>" } : []).Concat(entries.Any(e => e.Kind == EmittedBindingKind.Event) ? new[] { "<mutex>" } : [])
            .Concat(entries.Any(e => e.Kind == EmittedBindingKind.Event && InstanceEventInterop.UsesLifetime((FieldDescriptor)e.Native)) ? new[] { "\"Engine/Core/DelegateBindings.h\"", "<unordered_map>", "<atomic>" } : []).Distinct(StringComparer.Ordinal)) code.AppendLine("#include " + include);
        if (entries.Any(e => e.Kind == EmittedBindingKind.Event && InstanceEventInterop.UsesLifetime((FieldDescriptor)e.Native)))
            code.AppendLine("#if !FLAX_DELEGATE_BINDINGS_ENABLED\n#error \"V2 lifetime-aware events require the supported Win64 Delegate lifetime bridge and C++ exceptions\"\n#endif");
        if (requirements.Libraries.Count > 0)
        {
            code.AppendLine("#if defined(_MSC_VER)");
            foreach (string library in requirements.Libraries) code.AppendLine("#pragma comment(lib, \"" + library + "\")");
            code.AppendLine("#endif");
        }
        foreach (string declaration in requirements.Declarations) code.AppendLine(declaration);
        code.AppendLine("""
            // Projects may provide platform-specific overrides before this file.
            #ifndef DLLExport
            #if defined(_WIN32) || defined(__CYGWIN__) || (defined(PLATFORM_PS4) && PLATFORM_PS4)
            #define DLLExport __declspec(dllexport)
            #elif defined(__clang__) || defined(__GNUC__)
            #define DLLExport __attribute__((visibility("default"), used))
            #else
            #error "Unsupported export platform/compiler. Define DLLExport for this target."
            #endif
            #endif

            #ifndef CallDeclaraton
            #if defined(_MSC_VER)
            #define CallDeclaraton __cdecl
            #elif (defined(__clang__) || defined(__GNUC__)) && defined(__i386__)
            #define CallDeclaraton __attribute__((cdecl))
            #elif defined(__clang__) || defined(__GNUC__)
            // These targets use their native C ABI; an x86 cdecl attribute is unnecessary.
            #define CallDeclaraton
            #else
            #error "Unsupported calling convention. Define CallDeclaraton for this target."
            #endif
            #endif
            """);
        foreach (string support in codecs.Select(c => c.Bodies?.NativeSupport ?? "").Where(s => s.Length != 0).Distinct())
            code.AppendLine(support.Replace("{Support}", SupportName(support)));
        if (codecs.Any(c => c.Bodies != null)) code.AppendLine("""
            namespace {
            template<class F> struct VETConverterScopeExit {
                F action; bool active = true;
                explicit VETConverterScopeExit(F&& value) : action(std::move(value)) {}
                VETConverterScopeExit(const VETConverterScopeExit&) = delete;
                VETConverterScopeExit(VETConverterScopeExit&& other) : action(std::move(other.action)), active(other.active) { other.active = false; }
                ~VETConverterScopeExit() noexcept { if (active) { try { action(); } catch (...) {} } }
            };
            template<class F> VETConverterScopeExit<F> VETConverterFinally(F&& action) { return VETConverterScopeExit<F>(std::forward<F>(action)); }
            }
            """);
        foreach (var codec in codecs)
            if (codec.IsAlias) code.AppendLine("static_assert(std::is_same<" + codec.NativeCppType + ", " + codec.AbiCppType + Qualifiers(codec.NativeQualifiers) + ">::value, \"Registered alias does not match its declared native type\");");
            else code.AppendLine("static_assert(std::is_const<" + codec.NativeCppType + ">::value == " + ((codec.NativeQualifiers & TypeQualifiers.Const) != 0 ? "true" : "false") + " && std::is_volatile<" + codec.NativeCppType + ">::value == " + ((codec.NativeQualifiers & TypeQualifiers.Volatile) != 0 ? "true" : "false") + ", \"Registered native converter qualifiers do not match\");");
        // Verify every inline/shared layout against the compiler actually building the glue.
        // This also catches packing/alignment supplied through includes, macros or build flags.
        var layoutCalculator = new NativeLayout();
        var verifiedLayouts = new HashSet<StructDescriptor>();
        int accessInsertion = code.Length;
        Dictionary<TypeDescriptor, List<string>> privateAccessors = [];
        foreach (var entry in entries)
        {
            if (entry.Native is StructDescriptor structure && entry.StorageLayout != null) VerifyLayout(structure, entry.StorageLayout);
            foreach (var type in new[] { entry.NativeType, entry.NativeFunction?.ReturnType }.Concat(entry.NativeFunction?.Parameters ?? [])) VerifyReference(type);
        }
        void VerifyReference(TypeInfo? type)
        {
            if (type is ReferenceTypeInfo reference) { VerifyReference(reference.Referent); return; }
            if (type is not NamedTypeInfo { Converter.SharedReference: true } named) return;
            if ((named.Specialization?.Concrete ?? named.Symbol?.Definition) is StructDescriptor structure && layoutCalculator.Struct(structure) is { } storage)
                VerifyLayout(structure, storage);
        }
        void VerifyLayout(StructDescriptor structure, NativeStructLayout storage)
        {
            if (!verifiedLayouts.Add(structure)) return;
            string type = "VETVerifiedLayout_" + Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(structure.NativeQualifiedName + "|" + structure.SourceFile?.Path)))[..16];
            code.AppendLine($"using {type} = ::{structure.NativeQualifiedName};");
            code.AppendLine($"static_assert(std::is_standard_layout<{type}>::value, \"VET shared layout requires a standard-layout native type\");");
            code.AppendLine($"static_assert(sizeof({type}) == {storage.Storage.Size}, \"VET shared layout size mismatch: {structure.NativeQualifiedName}\");");
            code.AppendLine($"static_assert(alignof({type}) == {storage.Storage.Alignment}, \"VET shared layout alignment mismatch: {structure.NativeQualifiedName}\");");
            foreach (var field in storage.Offsets.Where(pair => pair.Key.Access == "public"))
                code.AppendLine($"static_assert(offsetof({type}, {field.Key.Name.text}) == {field.Value}, \"VET shared layout field offset mismatch: {structure.NativeQualifiedName}::{field.Key.Name.text}\");");
        }
        foreach (var entry in entries)
        {
            if (entry.Kind == EmittedBindingKind.Type && entry.Native is ClassDescriptor scriptingType && HandleNETcpp.IsScriptingObject(scriptingType) && !HandleNETcpp.CanCast(scriptingType))
            { Error(entry, "Flax scripting object inheritance requires one public, non-virtual ScriptingObject base path."); continue; }
            if (entry.Kind == EmittedBindingKind.Field && entry.Native is FieldDescriptor dataField && dataField.Parent is StructDescriptor structure && !NativeLayout.IsStatic(dataField))
            {
                if (structure.MarshalAsType != null) continue; // Managed data fields cross through the explicit surrogate, not native storage.
                if (entries.Single(e => e.Kind == EmittedBindingKind.Type && e.Native == structure).StorageLayout == null)
                    Error(entry, $"Cannot expose struct field '{dataField.QualifiedName}' without an established native storage layout.");
                continue;
            }
            if (entry.Kind == EmittedBindingKind.Type && entry.Native is ClassDescriptor derived && entry.ClassBase is { } baseClass)
            { EmitUpcast(entry, derived, baseClass); continue; }
            if (entry.Kind is EmittedBindingKind.Type or EmittedBindingKind.Enum or EmittedBindingKind.OpaqueType or EmittedBindingKind.Property or EmittedBindingKind.Event) continue;
            if (entry.Kind != EmittedBindingKind.Function || entry.Native is not FunctionDescriptor function || entry.NativeFunction is not { } signature)
            {
                string operation = entry.Kind switch
                {
                    EmittedBindingKind.Field => $"Cannot generate native access to exported field '{entry.Native.QualifiedName}' ({entry.NativeType?.Display ?? "unknown type"}). Direct field read/write wrappers are not implemented yet. Expose existing getter/setter methods using [[Export, Getter, Setter]], or implement the field-access converter.",
                    EmittedBindingKind.Destructor => $"Cannot expose '{entry.Native.QualifiedName}' as a direct destructor call. Export class constructors to use generated IDisposable cleanup, or provide a native factory with an explicit lifetime policy. Remove [[Export]] from the destructor itself.",
                    _ => $"Cannot generate native wrapper for exported {entry.Kind} '{entry.Native.QualifiedName}' (C#: {entry.ManagedDeclaration}). This emitted member kind has no native generation handler yet. Implement its handler before exporting it to C++."
                };
                Error(entry, operation); continue;
            }
            bool privateAccess = function.Access != "public" && function.Parent is TypeDescriptor;
            if (privateAccess && (function.Parent is not TypeDescriptor privateOwner || !HasFriendAccess(privateOwner) || signature.IsConstructor))
            { Error(entry, function.AccessedField is { } inaccessible ? $"Cannot access non-public native field '{inaccessible.QualifiedName}'. Expose public accessors or provide generated friendship." : "Native wrapper cannot access a non-public method without generated friendship."); continue; }
            if (signature.IsConstructor && function.Parent is ClassDescriptor scriptConstructor && HandleNETcpp.IsScriptingObject(scriptConstructor))
            {
                Error(entry, scriptConstructor.Api.NoSpawn
                    ? $"Flax scripting object '{scriptConstructor.QualifiedName}' is marked API_CLASS(NoSpawn): its scripting spawn handler cannot create an instance. No constructor wrapper is generated. Use an existing instance or an engine-managed factory; existing-instance method bindings remain available."
                    : "Flax scripting object construction belongs to Flax's managed/native spawning path; no raw new/delete wrapper is generated.");
                continue;
            }
            if (new[] { signature.ReturnType }.Concat(signature.Parameters).OfType<TypeInfo>().Select(HandleNETcpp.Use).OfType<HandleNETcpp.ObjectUse>().Any(use => !HandleNETcpp.CanCast(use.Type)))
            { Error(entry, "Flax scripting object conversion requires one public, non-virtual ScriptingObject base path."); continue; }
            if (signature.IsConstructor && function.Parent is not ClassDescriptor)
            { Error(entry, "Native construction of managed structs requires a placement-construction and destruction policy. Only exported class constructors are supported currently."); continue; }
            if (signature.IsConstructor && ((ClassDescriptor)function.Parent!).Functions.Any(other =>
                other != function && other.TypeInfo is { IsConstructor: true } sibling && sibling.Parameters.Count == signature.Parameters.Count &&
                sibling.Parameters.Zip(signature.Parameters).All(p => BindingSignaturePass.Canonical(p.First is ReferenceTypeInfo a ? a.Referent : p.First, true) == BindingSignaturePass.Canonical(p.Second is ReferenceTypeInfo b ? b.Referent : p.Second, true)) &&
                sibling.Parameters.Zip(signature.Parameters).Any(p => (p.First is ReferenceTypeInfo) != (p.Second is ReferenceTypeInfo))))
            { Error(entry, $"Cannot unambiguously call native constructor '{function.QualifiedName}': overlapping value/reference constructor overloads cannot be selected through a function pointer. Expose a named native factory for this overload."); continue; }
            if (signature.ReferenceQualifier != null || (!signature.IsConstructor && signature.ReturnType == null))
            { Error(entry, "Native construction/ref-qualified receiver requires an explicit lifetime/receiver backend."); continue; }
            if (signature.IsConstructor && !EnsureRelease((ClassDescriptor)function.Parent!, entry)) continue;
            TypeInfo returnType = signature.IsConstructor
                ? new PointerTypeInfo { Pointee = new NamedTypeInfo { Name = function.Parent!.QualifiedName, Resolution = TypeResolution.Resolved, Symbol = ((ClassDescriptor)function.Parent).TypeSymbol } }
                : signature.ReturnType!;
            bool copyReferenceReturn = entry.CopyReferenceReturn || returnType is ReferenceTypeInfo { Referent: NamedTypeInfo { Converter.Bodies.CopyReferenceReturns: true } };
            TypeInfo convertedReturnType = copyReferenceReturn ? ((ReferenceTypeInfo)returnType).Referent : returnType;
            if (!copyReferenceReturn && returnType is ReferenceTypeInfo && entry.Parameters!.Any(p => p.Passing == ParameterPassing.LValueReference && (p.Direction != ParameterDirection.Ref || p.CopyInOutReference)))
            { Error(entry, "Reference return may alias an In/Out temporary. A lifetime policy is required before emitting this wrapper."); continue; }
            if (!Supported(returnType, true) || signature.Parameters.Any(t => !Supported(t, false)))
            {
                var unsupported = (Supported(returnType, true) ? Enumerable.Empty<TypeInfo>() : [returnType])
                    .Concat(signature.Parameters.Where(type => !Supported(type, false)))
                    .Select(DiagnosticType).Distinct(StringComparer.Ordinal);
                string diagnosticParameters = string.Join(", ", signature.Parameters.Select((type, i) => DiagnosticType(type) +
                    (i < function.Fields.Count && !function.Fields[i].IsUnnamed ? " " + function.Fields[i].Name.text : "")));
                string qualifiers = Qualifiers(signature.Qualifiers) + (signature.ReferenceQualifier == ReferenceKind.LValue ? " &" : signature.ReferenceQualifier == ReferenceKind.RValue ? " &&" : "");
                Error(entry, function.AccessedField is { } field
                    ? $"Cannot bind exported field '{field.QualifiedName}' ({field.TypeInfo?.Display}). Its value requires an aggregate/callback/type converter; native field access currently supports scalars and raw pointers."
                    : "Cannot bind native function: " +
                      (signature.IsConstructor ? "" : DiagnosticType(returnType) + " ") + function.NativeQualifiedName + "(" + diagnosticParameters + ")" + qualifiers + ";\n" +
                      "No supported ABI converter is available for:\n" + string.Join("\n", unsupported));
                continue;
            }
            string key = function.Binding?.NativeSignature ?? function.QualifiedName;
            if (protectedBindings) key += "|return:" + BindingSignaturePass.Canonical(returnType, false) + "|directions:" + string.Join(",", entry.Parameters!.Select(p => p.Direction));
            string hash = Digest(key);
            string readable = ReadableName(function.QualifiedName);
            string name = protectedBindings ? "B" + hash : naming == NativeEntryPointNaming.Hashed ? "VET_" + hash : "Internal_" + readable + (readableCounts[readable] > 1 ? "__" + hash : "");
            if (!usedNames.Add(name))
            { Error(entry, $"Native entry-point name collision: '{name}'. Use distinct native signatures or another naming mode."); continue; }
            bool instance = !signature.IsConstructor && function.Parent is TypeDescriptor && !BindingSignaturePass.IsStatic(function);
            bool scriptingReceiver = instance && function.Parent is ClassDescriptor receiverClass && HandleNETcpp.IsScriptingObject(receiverClass);
            bool scriptingReturn = HandleNETcpp.SupportedUse(returnType);
            string owner = "::" + (function.Parent?.NativeQualifiedName ?? "");
            string cv = Qualifiers(signature.Qualifiers);
            List<string> parameters = [], checks = [], setup = [], args = [], finish = [];
            if (scriptingReceiver) setup.Add(HandleNETcpp.ReceiverSetup((ClassDescriptor)function.Parent!));
            int converterIndex = 0;
            string ConverterName(string stem) => "__vet_" + stem + "_" + converterIndex++;
            bool hasBodies = Converters(returnType).Any(c => c.Bodies != null) || signature.Parameters.SelectMany(Converters).Any(c => c.Bodies != null);
            if (hasBodies) setup.Add("bool __vetCallSucceeded = false; (void)__vetCallSucceeded;");
            (string Expression, string Complete) EmitConverter(ConverterBodies converter, Func<ConverterEmitContext, ConverterBody> emit, string value, ParameterDirection? direction, List<string> lines)
            {
                string complete = ConverterName("complete");
                var conversion = emit(new(value, direction, ConverterName, complete, "__vetCallSucceeded", SupportName(converter.NativeSupport)));
                if (string.IsNullOrWhiteSpace(conversion.Expression)) throw new ArgumentException("Converter returned an empty native expression for " + function.QualifiedName);
                lines.Add("bool " + complete + " = false;");
                if (conversion.Declarations.Length != 0) lines.Add(conversion.Declarations);
                if (conversion.Cleanup.Length != 0) lines.Add("auto " + ConverterName("cleanup") + " = VETConverterFinally([&]() { " + conversion.Cleanup + " });");
                if (conversion.Setup.Length != 0) lines.Add(conversion.Setup);
                return (conversion.Expression, complete + " = true; (void)" + complete + ";");
            }
            if (instance) { parameters.Add(scriptingReceiver ? HandleNETcpp.ReceiverParameter((ClassDescriptor)function.Parent!) : owner + cv + "* self"); checks.Add("!self"); }
            if (instance && !scriptingReceiver) checks.Add($"reinterpret_cast<std::uintptr_t>(self) % alignof({owner}) != 0");
            for (int i = 0; i < signature.Parameters.Count; i++)
            {
                var type = signature.Parameters[i];
                string arg = "arg" + i;
                if (entry.Parameters![i].InputPointerArray != null)
                { PointerArrayInterop.EmitNative(entry.Parameters[i], arg, parameters, args, checks, setup); continue; }
                if (RuntimeHandleInterop.Kind(type) != null)
                {
                    parameters.Add("void* " + arg);
                    args.Add("reinterpret_cast<" + Native(type) + ">(" + arg + ")");
                    continue;
                }
                if (HandleNETcpp.SupportedUse(type))
                {
                    var use = HandleNETcpp.Use(type)!;
                    parameters.Add(HandleNETcpp.ParameterType(type) + " " + arg);
                    if (use.Reference) checks.Add("!" + arg);
                    if (use.Slot)
                    {
                        string local = "scriptArg" + i;
                        var direction = entry.Parameters![i].Direction;
                        string localType = TemplateInstantiationPass.Native(((ReferenceTypeInfo)type).Referent);
                        setup.Add(localType + " " + local + " = " + (direction == ParameterDirection.Out ? "nullptr" : HandleNETcpp.ToNative(type, "*" + arg)) + ";");
                        args.Add(local);
                        if (direction != ParameterDirection.In) finish.Add("*" + arg + " = " + HandleNETcpp.ToRoot(use.Type, local) + ";");
                    }
                    else args.Add(HandleNETcpp.ToNative(type, arg));
                    continue;
                }
                if (type is ReferenceTypeInfo { ReferenceKind: ReferenceKind.RValue, Referent: NamedTypeInfo { Converter.Bodies: { MoveInputs: true } moveBodies } moved })
                {
                    parameters.Add(Wire(moved) + " " + arg);
                    var converted = EmitConverter(moveBodies, moveBodies.AbiToNative, arg, ParameterDirection.In, setup);
                    string local = ConverterName("moveValue");
                    setup.Add("auto " + local + " = " + converted.Expression + ";"); setup.Add(converted.Complete);
                    args.Add("std::move(" + local + ")");
                    continue;
                }
                if (type is ReferenceTypeInfo reference)
                {
                    bool marshalledRef = entry.Parameters![i].CopyInOutReference;
                    bool directReadOnly = entry.Parameters![i].DirectReadOnlyReference;
                    bool shared = (directReadOnly || entry.Parameters![i].Direction == ParameterDirection.Ref) && reference.Referent is NamedTypeInfo { Converter.SharedReference: true };
                    parameters.Add((shared ? Native(reference.Referent) : Wire(reference.Referent, true)) + "* " + arg); checks.Add("!" + arg);
                    var direction = entry.Parameters![i].Direction;
                    if (direction == ParameterDirection.Ref && !marshalledRef || directReadOnly)
                        checks.Add($"reinterpret_cast<std::uintptr_t>({arg}) % alignof({Native(reference.Referent)}) != 0");
                    if (direction == ParameterDirection.Ref && !marshalledRef || directReadOnly) args.Add("*" + arg);
                    else
                    {
                        // In/converted Ref owns an input copy; Out starts empty. Ref/Out copy back on success.
                        var codec = (reference.Referent as NamedTypeInfo)?.Converter;
                        (string Expression, string Complete)? inputBody = codec?.Bodies != null && direction != ParameterDirection.Out ? EmitConverter(codec.Bodies, codec.Bodies.AbiToNative, "*" + arg, direction, setup) : null;
                        string initial = inputBody?.Expression ?? (direction != ParameterDirection.Out ? FromWire(reference.Referent, "*" + arg) : codec?.Bodies != null ? codec.NativeDefault ?? Native(reference.Referent) + "{}" : codec is { IsAlias: false } ? codec.NativeDefault ?? codec.ToNative(codec.AbiCppType + "{}") : reference.Referent is PointerTypeInfo ? "static_cast<" + Native(reference.Referent) + ">(nullptr)" : Native(reference.Referent) + "{}");
                        setup.Add("auto local" + i + " = " + initial + ";");
                        if (inputBody != null) setup.Add(inputBody.Value.Complete);
                        args.Add("local" + i);
                        if (direction == ParameterDirection.Out || marshalledRef)
                        {
                            var outputBody = codec?.Bodies == null ? ((string Expression, string Complete)?)null : EmitConverter(codec.Bodies, codec.Bodies.NativeToAbi, "local" + i, direction, finish);
                            finish.Add("*" + arg + " = " + (outputBody?.Expression ?? ToWire(reference.Referent, "local" + i)) + ";");
                            if (outputBody != null) finish.Add(outputBody.Value.Complete);
                        }
                    }
                }
                else
                {
                    parameters.Add(Wire(type) + " " + arg);
                    if (type is NamedTypeInfo { Converter.Bodies: { } bodies })
                    {
                        var converted = EmitConverter(bodies, bodies.AbiToNative, arg, ParameterDirection.In, setup);
                        string local = ConverterName("value"); setup.Add("auto " + local + " = " + converted.Expression + ";"); setup.Add(converted.Complete); args.Add(local);
                    }
                    else args.Add(FromWire(type, arg));
                }
            }
            bool isVoid = returnType is NamedTypeInfo { Name: "void" } or NamedTypeInfo { Converter.AbiCppType: "void", Converter.IsAlias: true };
            if (signature.IsConstructor)
                for (int i = 0; i < args.Count; ++i) args[i] = "static_cast<" + Native(signature.Parameters[i]) + ">(" + args[i] + ")";
            if (!isVoid)
            {
                string outputType = RuntimeHandleInterop.Kind(returnType) != null ? "void*" : scriptingReturn ? HandleNETcpp.ReturnType(returnType) : copyReferenceReturn ? Wire(convertedReturnType) : returnType is ReferenceTypeInfo reference ? Native(reference.Referent) + "*" : Wire(returnType);
                parameters.Add(outputType + "* result"); checks.Add("!result");
            }
            string nativeParameters = string.Join(", ", signature.Parameters.Select(Native));
            string pointer = instance ? $"{Native(returnType)} ({owner}::*)({nativeParameters}){cv}" : $"{Native(returnType)} (*)({nativeParameters})";
            string target = instance ? (function.NativeDispatchInterface is { } dispatchInterface ? "::" + dispatchInterface.NativeQualifiedName : owner) + "::" + function.Name.text : "::" + function.NativeQualifiedName;
            // Flax platform facades call the selected implementation (Platform)
            // rather than the declaration's base class (PlatformBase).
            if (!instance && function.Parent is ClassDescriptor { Api.NativeInvokeUseName: true } facade)
            {
                string scope = facade.Parent is TypeDescriptor enclosing ? enclosing.NativeQualifiedName : string.Concat(facade.NamespaceTokens.Select(t => t.text));
                target = "::" + (scope.Length == 0 ? "" : scope + "::") + HandleNetCS.SimpleName(facade) + "::" + function.Name.text;
            }
            string receiver = scriptingReceiver ? "nativeSelf" : "self";
            string? privateTarget = null;
            string? arrayAssertion = null;
            if (privateAccess)
            {
                var accessOwner = (TypeDescriptor)function.Parent!;
                if (!privateAccessors.TryGetValue(accessOwner, out var accessors)) privateAccessors.Add(accessOwner, accessors = []);
                string accessor = "Member_" + hash;
                string expression = function.AccessedField is { } field ? "&::" + (field.NativeArrayField ?? field).NativeQualifiedName : $"static_cast<{pointer}>(&{target})";
                accessors.Add($"static auto {accessor}() -> decltype({expression}) {{ return {expression}; }}");
                privateTarget = $"::VETBindingAccess<{owner}>::{accessor}()";
            }
            string call = signature.IsConstructor ? "new " + owner + "(" + string.Join(", ", args) + ")" : instance ? "(" + receiver + "->*target)(" + string.Join(", ", args) + ")" : "target(" + string.Join(", ", args) + ")";
            if (function.NativeDefaultFactory) call = owner + "{}";
            if (function.AccessedField is { } accessed)
            {
                var sourceField = accessed.NativeArrayField ?? accessed;
                string address = instance ? receiver + "->" + sourceField.Name.text : "::" + sourceField.NativeQualifiedName;
                if (privateTarget != null) address = instance ? "(" + receiver + "->*" + privateTarget + ")" : "(*" + privateTarget + ")";
                if (accessed.NativeArrayField != null)
                {
                    arrayAssertion = $"static_assert(std::extent<typename std::remove_reference<decltype({address})>::type>::value == {accessed.NativeArrayCount}, \"VET fixed-array extent differs from the compiler\");";
                    address += "[" + accessed.NativeArrayIndex + "]";
                }
                call = function.IsFieldSetter && sourceField.TypeInfo is NamedTypeInfo { Converter.Bodies.AssignNativeValue: { } assignFields }
                    ? assignFields(address, args[0]) : function.IsFieldSetter ? address + " = " + args[0] : address;
            }
            var route = routes[entry];
            string methodName = "Internal_" + function.Name.text;
            if (readableCounts[readable] > 1 || route.Scope.Children.ContainsKey(methodName)) methodName += "__" + hash;
            StringBuilder body = new();
            if (!protectedBindings) body.AppendLine("// " + function.QualifiedName);
            body.AppendLine(protectedBindings
                ? $"extern \"C\" DLLExport int CallDeclaraton {name}({string.Join(", ", parameters)}) {{"
                : $"static int CallDeclaraton {methodName}({string.Join(", ", parameters)}) {{");
            if (checks.Count != 0) body.AppendLine("    if (" + string.Join(" || ", checks) + ") return 1;");
            if (signature.IsConstructor) body.AppendLine("    *result = nullptr;");
            body.AppendLine("    try {");
            if (signature.IsConstructor)
                body.AppendLine($"\n#if !defined(__cpp_aligned_new) || __cpp_aligned_new < 201606L\nstatic_assert(alignof({owner}) <= alignof(std::max_align_t), \"VET over-aligned construction requires aligned new or an explicit allocator\");\n#endif");
            foreach (string line in setup) body.AppendLine("        " + line);
            if (arrayAssertion != null) body.AppendLine("        " + arrayAssertion);
            if (!signature.IsConstructor && function.AccessedField == null && !function.NativeDefaultFactory) body.AppendLine($"        auto target = " + (privateTarget ?? $"static_cast<{pointer}>(&{target})") + ";");
            string constructed = signature.IsConstructor ? ConverterName("constructed") : "";
            if (signature.IsConstructor) body.AppendLine($"        std::unique_ptr<{owner}> {constructed}({call});");
            else if (scriptingReturn) body.AppendLine("        *result = " + HandleNETcpp.ReturnValue(returnType, call) + ";");
            else if (convertedReturnType is NamedTypeInfo { Converter.IsAlias: false })
            {
                body.AppendLine("        auto&& nativeResult = " + call + ";");
                if (convertedReturnType is NamedTypeInfo { Converter.Bodies: { } returnBodies })
                {
                    List<string> lines = [];
                    var converted = EmitConverter(returnBodies, returnBodies.NativeToAbi, "nativeResult", null, lines);
                    foreach (string line in lines) body.AppendLine("        " + line);
                    body.AppendLine("        *result = " + converted.Expression + ";"); body.AppendLine("        " + converted.Complete);
                }
                else body.AppendLine("        *result = " + ToWire(convertedReturnType, "nativeResult") + ";");
            }
            else if (RuntimeHandleInterop.Kind(returnType) != null) body.AppendLine("        *result = const_cast<void*>(static_cast<const void*>(" + call + "));");
            else body.AppendLine("        " + (isVoid ? call : "*result = " + (returnType is ReferenceTypeInfo ? "std::addressof(" + call + ")" : call)) + ";");
            foreach (string line in finish) body.AppendLine("        " + line);
            if (signature.IsConstructor) body.AppendLine($"        *result = {constructed}.release();");
            if (hasBodies) body.AppendLine("        __vetCallSucceeded = true;");
            body.AppendLine("        return 0;\n    } catch (...) { return 2; }\n}");
            if (protectedBindings)
            {
                code.Append(body);
                result.WrapperMethods.Add(entry, "::" + name);
                result.EntryPoints.Add(entry, name);
                continue;
            }
            route.Scope.Methods.Add(body.ToString());
            string qualifiedMethod = route.Qualified + "::" + methodName;
            result.WrapperMethods.Add(entry, qualifiedMethod);
            bridges.AppendLine($"extern \"C\" DLLExport int CallDeclaraton {name}({string.Join(", ", parameters)}) {{");
            List<string> forwarded = [];
            if (instance) forwarded.Add("self");
            forwarded.AddRange(Enumerable.Range(0, signature.Parameters.Count).SelectMany(i => entry.Parameters![i].InputPointerArray != null ? new[] { "arg" + i, "arg" + i + "Length" } : new[] { "arg" + i }));
            if (!isVoid) forwarded.Add("result");
            bridges.AppendLine($"    return {qualifiedMethod}({string.Join(", ", forwarded)});\n}}");
            result.EntryPoints.Add(entry, name);
        }
        if (result.Success) {
            foreach (var entry in entries.Where(e => e.Kind == EmittedBindingKind.Event && e.Native.Access != "public"))
            {
                var field = (FieldDescriptor)entry.Native;
                var accessOwner = (TypeDescriptor)field.Parent!;
                if (!privateAccessors.TryGetValue(accessOwner, out var accessors)) privateAccessors.Add(accessOwner, accessors = []);
                string expression = "&::" + field.NativeQualifiedName;
                accessors.Add($"static auto {EventInterop.Accessor(field)}() -> decltype({expression}) {{ return {expression}; }}");
            }
            StringBuilder accessCode = new();
            foreach (var (type, accessors) in privateAccessors)
                accessCode.AppendLine($"template<> struct VETBindingAccess<::{type.NativeQualifiedName}> {{\n" + string.Join("\n", accessors) + "\n};");
            code.Insert(accessInsertion, accessCode.ToString());
            if (!protectedBindings) { root.Emit(code); code.Append(bridges); } EventInterop.EmitNative(code, entries, result, headerPath, naming, engineVersion); result.Source = code.ToString(); }
        else { result.EntryPoints.Clear(); result.WrapperMethods.Clear(); result.Upcasts.Clear(); result.Releases.Clear(); }
        return result;
        void Error(EmittedBinding entry, string message) => result.Diagnostics.Add(new LogEntry(message,
            entry.Native is FunctionDescriptor { AccessedField: { } field } ? field.Name : entry.Native.Name, LogEntryType.Error, "VETCPP001", entry.Native.DiagnosticFile?.Path));
        string Digest(string key)
        {
            if (protectedBindings) key = "ProtectedBindingsV1|" + engineVersion!.Length + ":" + engineVersion + "|" + key;
            return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(key)))[..(protectedBindings ? 32 : 16)];
        }
        void EmitUpcast(EmittedBinding entry, ClassDescriptor derived, ClassDescriptor baseClass)
        {
            var baseInfo = derived.BaseTypes.Single(b => b.Type is NamedTypeInfo { Symbol.Definition: ClassDescriptor c } && c == baseClass);
            if (baseInfo.Access != "public")
            { Error(entry, $"Cannot generate borrowed receiver conversion from '{derived.QualifiedName}' to '{baseClass.QualifiedName}': inheritance is {baseInfo.Access}. Public inheritance or a native friend converter is required."); return; }
            string hash = Digest("upcast|" + derived.QualifiedName + "|" + baseClass.QualifiedName);
            string name = protectedBindings ? "B" + hash : naming == NativeEntryPointNaming.Hashed ? "VET_" + hash : "Internal_" + ReadableName(derived.QualifiedName) + "Upcast__" + hash;
            if (!usedNames.Add(name)) { Error(entry, "Native upcast entry-point name collision: " + name); return; }
            string parameters = $"::{derived.NativeQualifiedName}* self, ::{baseClass.NativeQualifiedName}** result";
            string body = "if (!self || !result) return 1;\n*result = static_cast<::" + baseClass.NativeQualifiedName + "*>(self);\nreturn 0;";
            if (protectedBindings)
                code.AppendLine($"extern \"C\" DLLExport int CallDeclaraton {name}({parameters}) {{\n{body}\n}}");
            else
            {
                string method = "Internal_Upcast__" + hash;
                var route = routes[entry];
                route.Scope.Methods.Add($"static int CallDeclaraton {method}({parameters}) {{\n{body}\n}}");
                bridges.AppendLine($"extern \"C\" DLLExport int CallDeclaraton {name}({parameters}) {{\n    return {route.Qualified}::{method}(self, result);\n}}");
            }
            result.Upcasts.Add(entry, name);
        }
        bool EnsureRelease(ClassDescriptor cls, EmittedBinding constructor)
        {
            var typeEntry = entries.Single(e => e.Kind == EmittedBindingKind.Type && e.Native == cls);
            if (result.Releases.ContainsKey(typeEntry)) return true;
            var destructor = cls.Functions.FirstOrDefault(f => f.TypeInfo?.IsDestructor == true);
            if (destructor != null && (destructor.Access != "public" || destructor.TypeInfo!.SuffixTokens.Any(t => t == "delete")))
            { Error(constructor, $"Cannot own '{cls.QualifiedName}': its destructor must be public and not deleted. Expose a native factory with a custom lifetime policy instead."); return false; }
            string hash = Digest("delete|" + cls.QualifiedName);
            string name = protectedBindings ? "B" + hash : naming == NativeEntryPointNaming.Hashed ? "VET_" + hash : "Internal_" + ReadableName(cls.QualifiedName) + "Delete__" + hash;
            if (!usedNames.Add(name)) { Error(constructor, "Native delete entry-point name collision: " + name); return false; }
            string parameters = $"::{cls.NativeQualifiedName}* self";
            string body = "try { delete self; return 0; } catch (...) { return 2; }";
            if (protectedBindings) code.AppendLine($"extern \"C\" DLLExport int CallDeclaraton {name}({parameters}) {{\n    {body}\n}}");
            else
            {
                string method = "Internal_Delete__" + hash;
                var route = routes[typeEntry];
                route.Scope.Methods.Add($"static int CallDeclaraton {method}({parameters}) {{\n    {body}\n}}");
                bridges.AppendLine($"extern \"C\" DLLExport int CallDeclaraton {name}({parameters}) {{\n    return {route.Qualified}::{method}(self);\n}}");
            }
            result.Releases.Add(typeEntry, name); return true;
        }
    }

    static string ReadableName(string qualifiedName) => "CppGlue" + string.Concat(qualifiedName.Split("::").Select(part =>
        string.Concat(part.Select(c => char.IsAsciiLetterOrDigit(c) || c == '_' ? c : '_'))));
    internal static string SupportName(string source) => "VETConverterSupport_" + Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(source)))[..16];

    static string DiagnosticType(TypeInfo type)
    {
        try { return Native(type).TrimStart(':'); }
        catch (NotSupportedException) { return type.Display; }
    }

    static bool Supported(TypeInfo type, bool returns) => type switch
    {
        _ when RuntimeHandleInterop.Kind(type) != null => true,
        _ when HandleNETcpp.SupportedUse(type) => true,
        NamedTypeInfo { Converter: { } converter } => Supported(converter.AbiType(), returns),
        NamedTypeInfo n => n.Resolution == TypeResolution.Builtin && n.Arguments.Count == 0 &&
            n.Name is not ("wchar_t" or "char16_t" or "char32_t" or "long double" or "auto") && (returns || n.Name != "void"),
        PointerTypeInfo p => PointerTarget(p.Pointee),
        ReferenceTypeInfo r => (r.ReferenceKind == ReferenceKind.LValue || !returns && r.Referent is NamedTypeInfo { Converter.Bodies.MoveInputs: true }) && r.Referent is not ReferenceTypeInfo && Supported(r.Referent, false),
        _ => false
    };
    static bool PointerTarget(TypeInfo type) => type switch
    {
        NamedTypeInfo n => n.Converter != null || n.Specialization?.Concrete != null || n.Arguments.Count == 0 && n.Resolution is TypeResolution.Builtin or TypeResolution.Resolved,
        PointerTypeInfo p => PointerTarget(p.Pointee),
        _ => false
    };
    static string Qualifiers(TypeQualifiers cv) => ((cv & TypeQualifiers.Const) != 0 ? " const" : "") + ((cv & TypeQualifiers.Volatile) != 0 ? " volatile" : "");
    static string Native(TypeInfo type) => type.NativeAliasSpelling ?? (type switch
    {
        NamedTypeInfo n when n.Specialization?.Concrete != null || n.Symbol?.Definition is TypeDescriptor { ConcreteTemplate: not null } => TemplateInstantiationPass.Native(n),
        NamedTypeInfo n => (n.Converter?.NativeCppType ?? (n.Symbol != null ? "::" + n.Symbol.QualifiedName : n.Name)) + Qualifiers(n.Qualifiers),
        PointerTypeInfo p => Native(p.Pointee) + "*" + Qualifiers(p.Qualifiers),
        ReferenceTypeInfo r => Native(r.Referent) + (r.ReferenceKind == ReferenceKind.RValue ? "&&" : "&"),
        _ => throw new NotSupportedException(type.Display)
    });
    static string Wire(TypeInfo type, bool cv = false) => type is NamedTypeInfo { Converter: { } converter } n ? converter.AbiCppType + (cv ? Qualifiers(n.EffectiveQualifiers) : "") : !cv && type.NativeAliasSpelling != null ? type switch
    {
        NamedTypeInfo named when named.Specialization?.Concrete != null || named.Symbol?.Definition is TypeDescriptor { ConcreteTemplate: not null } => TemplateInstantiationPass.Native(named),
        NamedTypeInfo named => (named.Symbol != null ? "::" + named.Symbol.QualifiedName : named.Name),
        PointerTypeInfo pointer => Native(pointer.Pointee) + "*",
        _ => Native(type)
    } : Native(type);
    static string FromWire(TypeInfo type, string value) => type is NamedTypeInfo { Converter.IsAlias: false } n ? n.Converter.ToNative(value) : value;
    static string ToWire(TypeInfo type, string value) => type is NamedTypeInfo { Converter.IsAlias: false } n ? n.Converter.ToAbi(value) : value;
    internal static bool HasFriendAccess(TypeDescriptor type) => type.Declarations.OfType<RawDeclarationDescriptor>().Any(raw => raw.Name.text is
        "DECLARE_SCRIPTING_TYPE_MINIMAL" or "DECLARE_SCRIPTING_TYPE_STRUCTURE" or "DECLARE_SCRIPTING_TYPE_NO_SPAWN" or
        "DECLARE_SCRIPTING_TYPE" or "DECLARE_SCRIPTING_TYPE_WITH_CONSTRUCTOR_IMPL" or
        "DECLARE_SCENE_OBJECT" or "DECLARE_SCENE_OBJECT_ABSTRACT" or "DECLARE_SCENE_OBJECT_NO_SPAWN" or "DECLARE_ASSET_HEADER" or "DECLARE_BINARY_ASSET_HEADER");
    static IEnumerable<string> ConverterDefinitionHeaders(TypeInfo type) => type switch
    {
        NamedTypeInfo named => (named.Converter?.Bodies != null && named.Symbol?.Definition?.SourceFile?.Path is { } path ? new[] { path } : Array.Empty<string>())
            .Concat(named.Arguments.Where(argument => argument.Type != null).SelectMany(argument => ConverterDefinitionHeaders(argument.Type!))),
        PointerTypeInfo pointer => ConverterDefinitionHeaders(pointer.Pointee),
        ReferenceTypeInfo reference => ConverterDefinitionHeaders(reference.Referent),
        ArrayTypeInfo array => ConverterDefinitionHeaders(array.ElementType),
        _ => []
    };
    static IEnumerable<NativeTypeConverter> Converters(TypeInfo type) => type switch
    {
        NamedTypeInfo { Converter: { } converter } => [converter],
        PointerTypeInfo p => Converters(p.Pointee),
        ReferenceTypeInfo r => Converters(r.Referent),
        ArrayTypeInfo a => Converters(a.ElementType),
        _ => []
    };
}
