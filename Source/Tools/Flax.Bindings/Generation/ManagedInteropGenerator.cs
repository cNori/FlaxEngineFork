using System.Security.Cryptography;
using System.Text;
using VETools.Text;

namespace VETools.Generation;

internal sealed class ManagedInteropResult
{
    public string Source = "";
    public List<LogEntry> Diagnostics = [];
    public bool Success => !Diagnostics.Any(entry => entry.BlocksGeneration);
}

// Wire recorded emitted method spans; do not parse generated C# or re-run filtering.
internal static class ManagedInteropGenerator
{
    public static ManagedInteropResult Generate(CSharpGenerationResult managed, CppGenerationResult native, string libraryName)
    {
        if (!managed.Success || !native.Success) throw new InvalidOperationException("Interop generation requires successful managed and native emission.");
        if (string.IsNullOrWhiteSpace(libraryName)) throw new ArgumentException("An explicit native library name is required.", nameof(libraryName));
        ManagedInteropResult result = new();
        var entries = managed.Bindings.Entries;
        string suffix = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(libraryName + "\0" + native.InputHeader + "\0" + string.Join("|", native.EntryPoints.Values) + string.Join("|", native.Upcasts.Values) + string.Join("|", native.Releases.Values) + string.Join("|", native.Events.Values))))[..16];
        string imports = "__NativeImports_" + suffix;
        string lifetime = "__NativeLifetime_" + suffix;
        var typeBindings = entries.Where(e => e.Kind == EmittedBindingKind.Type).ToDictionary(e => e.Native);
        List<(int Start, int Length, string Text)> edits = [];
        StringBuilder declarations = new();
        HashSet<ClassDescriptor> handles = [];
        HashSet<ClassDescriptor> resolvingHandles = [];
        HashSet<TypeDescriptor> views = [];
        Dictionary<(string Source, string? Owner), ConverterBodies> converterSupport = [];
        string SupportName(string source) => "__VETConverterSupport_" + Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(source)))[..16] + "_" + suffix;
        string SupportPath(ConverterBodies converter) => (converter.ManagedSupportOwner?.Replace("global::", "") ?? "Interop") + "." + SupportName(converter.ManagedSupport);
        Dictionary<ClassDescriptor, string> releaseMethods = [];
        int index = 0;
        foreach (var pair in native.Events)
        {
            foreach (var converter in EventInterop.CopiedConverters([pair.Key]).Select(c => c.Bodies!))
                if (converter.ManagedSupport.Length != 0) converterSupport.TryAdd((converter.ManagedSupport, converter.ManagedSupportOwner), converter);
            var eventCode = EventInterop.EmitManaged(pair.Key, pair.Value, libraryName, suffix, SupportPath);
            edits.Add((pair.Key.ManagedSourceStart, pair.Key.ManagedSourceLength, eventCode));
        }
        foreach (var view in typeBindings.Values.Where(binding => binding.NativeAccessView))
            EnsureNativeView((TypeDescriptor)view.Native, view);
        // Project generation promises this constructor/lifetime ABI to consuming
        // headers. Emit it even for abstract/empty native base wrappers.
        foreach (var binding in typeBindings.Values.Where(b => b.Native is ClassDescriptor cls && managed.ProjectClassLifetimes.Contains(cls)))
            EnsureHandle((ClassDescriptor)binding.Native, binding);
        foreach (var pair in native.EntryPoints)
        {
            var entry = pair.Key;
            if (!entries.Contains(entry) || entry.Native is not FunctionDescriptor function || entry.ManagedSourceStart < 0 || entry.ManagedSourceLength == 0)
            { Error(entry, "Native entry point does not match a recorded managed method body."); continue; }
            if (function.Parent is InterfaceDescriptor)
            { Error(entry, "Interface dispatch requires a concrete native receiver implementation."); continue; }
            bool constructor = entry.NativeFunction?.IsConstructor == true;
            bool instance = !constructor && function.Parent is TypeDescriptor && !BindingSignaturePass.IsStatic(function);
            bool structure = instance && function.Parent is StructDescriptor;
            bool scriptingReceiver = instance && function.Parent is ClassDescriptor scriptingClass && HandleNETcpp.IsScriptingObject(scriptingClass);
            var receiverLayout = structure ? typeBindings[function.Parent!].StorageLayout : null;
            bool nativeView = structure && typeBindings[function.Parent!].NativeAccessView;
            if (nativeView && !EnsureNativeView((TypeDescriptor)function.Parent!, entry)) continue;
            if (structure && !nativeView && receiverLayout == null)
            { Error(entry, "Struct receiver has no established unmanaged layout; it cannot be pinned for native calls."); continue; }
            if (structure && !nativeView && receiverLayout!.Storage.Alignment > IntPtr.Size)
            { Error(entry, $"Struct receiver requires {receiverLayout.Storage.Alignment}-byte alignment; pinned managed storage cannot guarantee it. An aligned receiver converter is required."); continue; }
            if ((instance || constructor) && function.Parent is ClassDescriptor cls)
            {
                if (!HandleNETcpp.IsScriptingObject(cls) && !EnsureHandle(cls, entry)) continue;
            }
            string importName = "Call" + index++;
            List<string> importParams = [], callArgs = [], fixedLines = [], init = [], setup = [], copyback = [], cleanup = [];
            List<string> scriptingKeepAlive = [];
            List<(string Pointer, string Value, string Type, int Size)> referenceRoots = [];
            var referenceLayout = new NativeLayout();
            HashSet<string> used = entry.Parameters!.Select(p => p.ManagedName.TrimStart('@')).ToHashSet(StringComparer.Ordinal);
            string Unique(string stem) { while (!used.Add(stem)) stem += "_"; return stem; }
            string self = Unique("__self"), status = Unique("__status"), value = Unique("__result");
            string callSucceeded = Unique("__callSucceeded");
            bool hasBodies = false;
            string? retainedRoots = null;
            string RetainManaged(string expression)
            {
                if (retainedRoots == null)
                {
                    retainedRoots = Unique("__marshalRoots");
                    init.Add($"var {retainedRoots} = new global::System.Collections.Generic.List<object>();");
                    cleanup.Add($"global::System.GC.KeepAlive({retainedRoots});");
                }
                return retainedRoots + ".Add(" + expression + ");";
            }
            (string Expression, string Complete) EmitConverter(ConverterBodies converter, Func<ConverterEmitContext, ConverterBody> emit, string expression, ParameterDirection? direction, List<string> phase)
            {
                hasBodies = true;
                string complete = Unique("__converted");
                if (converter.ManagedSupport.Length != 0) converterSupport.TryAdd((converter.ManagedSupport, converter.ManagedSupportOwner), converter);
                var conversion = emit(new(expression, direction, Unique, complete, callSucceeded, SupportPath(converter), RetainManaged));
                if (string.IsNullOrWhiteSpace(conversion.Expression)) throw new ArgumentException("Converter returned an empty managed expression for " + function.QualifiedName);
                init.Add("bool " + complete + " = false;");
                if (conversion.Declarations.Length != 0) init.Add(conversion.Declarations);
                if (conversion.Setup.Length != 0) phase.Add(conversion.Setup);
                if (conversion.Cleanup.Length != 0) cleanup.Add(conversion.Cleanup);
                return (conversion.Expression, complete + " = true; _ = " + complete + ";");
            }
            if (instance)
            {
                importParams.Add("nint self");
                if (scriptingReceiver) callArgs.Add(HandleNetCS.ToNative("this"));
                else if (nativeView) callArgs.Add("__NativeAddress");
                else if (structure)
                {
                    string typeName = HandleNetCS.ManagedName(function.Parent!);
                    fixedLines.Add($"fixed ({typeName}* {self} = &this)");
                    callArgs.Add($"(nint){self}");
                    referenceRoots.Add((self, "this", typeName, receiverLayout!.Storage.Size));
                }
                else callArgs.Add("__NativeAddress");
            }
            foreach (var parameter in entry.Parameters!)
            {
                if (PointerArrayInterop.EmitManaged(parameter, entry.Parameters!, Unique, init, setup, fixedLines, importParams, callArgs, scriptingKeepAlive)) continue;
                if (RuntimeHandleInterop.Parameter(parameter, Unique, init, setup, cleanup, importParams, callArgs)) { hasBodies = true; continue; }
                if (HandleNetCS.EmitParameter(parameter, Unique, init, setup, importParams, callArgs, copyback, scriptingKeepAlive)) continue;
                if (!parameter.DirectReadOnlyReference && !(parameter.Direction == ParameterDirection.Ref && parameter.NativeType is ReferenceTypeInfo { Referent: NamedTypeInfo { Converter.SharedReference: true } }) && Bodies(parameter.NativeType) is { } converter)
                {
                    string wire = Unique("__wire");
                    string abi = converter.ManagedAbiType == "bool" ? "byte" : converter.ManagedAbiType;
                    init.Add(abi + " " + wire + " = default;");
                    if (parameter.Direction == ParameterDirection.Out) init.Add(parameter.ManagedName + " = default;");
                    else
                    {
                        var input = EmitConverter(converter, converter.ManagedToAbi, parameter.ManagedName, parameter.Direction, setup);
                        setup.Add(wire + " = " + (converter.ManagedAbiType == "bool" ? "(byte)((" + input.Expression + ") ? 1 : 0)" : input.Expression) + ";"); setup.Add(input.Complete);
                    }
                    bool byref = parameter.Passing == ParameterPassing.LValueReference;
                    importParams.Add(abi + (byref ? "* " : " ") + Unique("__arg"));
                    callArgs.Add((byref ? "&" : "") + wire);
                    if (parameter.Direction == ParameterDirection.Out || parameter.Direction == ParameterDirection.Ref && converter.CopyReferenceParameters)
                    {
                        string outputWire = wire;
                        string? inputWire = null;
                        if (parameter.Direction == ParameterDirection.Ref)
                        {
                            inputWire = Unique("__inputWire");
                            outputWire = Unique("__output");
                            init.Add(abi + " " + inputWire + " = default;");
                            init.Add(abi + " " + outputWire + " = default;");
                            setup.Add(inputWire + " = " + wire + ";");
                            copyback.Add(outputWire + " = " + wire + ";");
                        }
                        var output = EmitConverter(converter, converter.AbiToManaged, converter.ManagedAbiType == "bool" ? outputWire + " != 0" : outputWire, parameter.Direction, copyback);
                        if (inputWire != null)
                            cleanup.Add(outputWire + " = " + wire + " == " + inputWire + " ? default : " + wire + ";");
                        copyback.Add(parameter.ManagedName + " = " + output.Expression + ";"); copyback.Add(output.Complete);
                    }
                    continue;
                }
                string type = ParameterType(parameter.ManagedType);
                string raw = type == "bool" ? "byte" : type;
                if (parameter.Passing == ParameterPassing.LValueReference)
                {
                    string pointer = Unique("__arg");
                    importParams.Add(raw + "* " + pointer);
                    if (parameter.Direction == ParameterDirection.Out) init.Add(parameter.ManagedName + " = default;");
                    fixedLines.Add($"fixed ({type}* {pointer} = &global::System.Runtime.CompilerServices.Unsafe.AsRef(in {parameter.ManagedName}))");
                    callArgs.Add($"({raw}*){pointer}");
                    if (parameter.Direction == ParameterDirection.Ref && parameter.NativeType is ReferenceTypeInfo r && referenceLayout.Storage(r.Referent) is { } rootStorage)
                        referenceRoots.Add((pointer, parameter.ManagedName, type, rootStorage.Size));
                }
                else
                {
                    importParams.Add(raw + " " + Unique("__arg"));
                    callArgs.Add(type == "bool" ? $"(byte)({parameter.ManagedName} ? 1 : 0)" : parameter.ManagedName);
                }
            }
            bool scriptingReturn = entry.NativeType != null && HandleNETcpp.SupportedUse(entry.NativeType);
            bool referenceReturn = !entry.CopyReferenceReturn && !scriptingReturn && entry.NativeType is ReferenceTypeInfo && Bodies(entry.NativeType)?.CopyReferenceReturns != true;
            NativeStorage? returnedStorage = referenceReturn ? referenceLayout.Storage(((ReferenceTypeInfo)entry.NativeType!).Referent) : null;
            if (referenceReturn && (returnedStorage == null || !referenceRoots.Any(root => root.Size >= returnedStorage.Size)))
            {
                Error(entry, $"Unanchored native reference return in '{function.QualifiedName}'. A bare C# ref cannot retain a native owner's lifetime. Provide an explicit copy converter/value-returning wrapper, or return an alias into a managed Ref parameter or inline struct receiver.");
                continue;
            }
            bool isVoid = !constructor && entry.ManagedReturnType == "void";
            string returnType = entry.ManagedReturnType;
            var resultConverter = referenceReturn ? null : Bodies(entry.NativeType);
            string wireReturn = resultConverter?.ManagedAbiType ?? returnType;
            string rawReturn = constructor || referenceReturn || scriptingReturn || RuntimeHandleInterop.IsMapped(entry.NativeType, entry.ManagedReturnType) ? "nint" : wireReturn == "bool" ? "byte" : wireReturn;
            List<string> returnSetup = [];
            var convertedReturn = resultConverter == null ? ((string Expression, string Complete)?)null : EmitConverter(resultConverter, resultConverter.AbiToManaged, wireReturn == "bool" ? value + " != 0" : value, null, returnSetup);
            if (!isVoid) { importParams.Add(rawReturn + "* result"); callArgs.Add("&" + value); }
            declarations.AppendLine($"    [global::System.Runtime.InteropServices.DllImport({Literal(libraryName)}, EntryPoint = {Literal(pair.Value)}, ExactSpelling = true, CallingConvention = global::System.Runtime.InteropServices.CallingConvention.Cdecl)]");
            declarations.AppendLine($"    internal static extern int {importName}({string.Join(", ", importParams)});");
            string? runtimeResult = null;
            if (RuntimeHandleInterop.IsMapped(entry.NativeType, entry.ManagedReturnType) && RuntimeHandleInterop.Kind(entry.NativeType) is { } resultKind)
            { hasBodies = true; runtimeResult = RuntimeHandleInterop.Return(resultKind, value, Unique, init, returnSetup, cleanup); }
            StringBuilder body = new();
            string release = constructor ? ReleaseMethod((ClassDescriptor)function.Parent!, entry) : "";
            string constructorOwner = Unique("__constructorOwner"), constructorTransferred = Unique("__constructorTransferred");
            if (constructor)
                // Private forwarding uses native order and has no optional defaults.
                body.AppendLine($"internal static {lifetime} Create{importName}({string.Join(", ", entry.Parameters!.Select(p => p.ManagedType + " " + p.ManagedName))})");
            else
            {
                if (referenceReturn && structure && !nativeView)
                    body.AppendLine("[global::System.Diagnostics.CodeAnalysis.UnscopedRef]");
                body.AppendLine(entry.ManagedDeclaration);
            }
            body.AppendLine("{\n    unsafe\n    {");
            string lease = Unique("__lease");
            if (instance && !structure && !scriptingReceiver)
                body.AppendLine($"        bool {lease} = false;\n        try\n        {{\n            __NativeLifetime.DangerousAddRef(ref {lease});");
            foreach (string line in init) body.Append(Indent(line, "        "));
            if (constructor)
                body.AppendLine($"        nint {value} = 0;\n        {lifetime} {constructorOwner} = null;\n        bool {constructorTransferred} = false;\n        try\n        {{");
            bool scriptingLifetime = scriptingReceiver || scriptingKeepAlive.Count != 0;
            if (scriptingLifetime) body.AppendLine("        try\n        {");
            if (!isVoid && !constructor) body.AppendLine($"        {rawReturn} {value} = default;");
            if (hasBodies) body.AppendLine($"        bool {callSucceeded} = false;\n        try\n        {{");
            foreach (string line in setup) body.Append(Indent(line, "        "));
            foreach (string line in fixedLines) body.AppendLine("        " + line);
            body.AppendLine("        {");
            body.AppendLine($"            int {status} = global::{imports}.{importName}({string.Join(", ", callArgs)});");
            if (constructor)
                body.AppendLine($"            if ({value} != 0) {{ {constructorOwner} = new {lifetime}({value}, {release}); {value} = 0; }}");
            body.AppendLine($"            if ({status} != 0) throw new global::System.InvalidOperationException(\"Native binding call failed with status \" + {status});");
            if (hasBodies) body.AppendLine($"            {callSucceeded} = true; _ = {callSucceeded};");
            foreach (string line in copyback) body.Append(Indent(line, "            "));
            if (constructor)
            {
                body.AppendLine($"            if ({constructorOwner} == null) throw new global::System.InvalidOperationException(\"Native constructor returned null.\");");
            }
            else if (!isVoid)
            {
                if (runtimeResult != null)
                {
                    foreach (string line in returnSetup) body.Append(Indent(line, "            "));
                    body.AppendLine("            return " + runtimeResult + ";");
                }
                else if (scriptingReturn) body.AppendLine("            return " + HandleNetCS.FromNative(entry.NativeType!, value) + ";");
                else if (referenceReturn)
                {
                    string referent = returnType.StartsWith("ref readonly ") ? returnType[13..] : returnType[4..];
                    body.AppendLine($"            if ({value} == 0) throw new global::System.InvalidOperationException(\"Native binding returned a null reference.\");");
                    body.AppendLine($"            if ((nuint){value} % {returnedStorage!.Alignment}U != 0) throw new global::System.InvalidOperationException(\"Native reference return is misaligned.\");");
                    foreach (var root in referenceRoots.Where(root => root.Size >= returnedStorage.Size))
                    {
                        string offset = Unique("__referenceOffset");
                        body.AppendLine($"            nuint {offset} = unchecked((nuint){value} - (nuint){root.Pointer});");
                        body.AppendLine($"            if ({offset} <= {root.Size - returnedStorage.Size}U)");
                        body.AppendLine($"                return ref global::System.Runtime.CompilerServices.Unsafe.As<byte, {referent}>(ref global::System.Runtime.CompilerServices.Unsafe.AddByteOffset(ref global::System.Runtime.CompilerServices.Unsafe.As<{root.Type}, byte>(ref {root.Value}), (nint){offset}));");
                    }
                    body.AppendLine("            throw new global::System.InvalidOperationException(\"Native reference return is outside caller-owned managed storage.\");");
                }
                else if (convertedReturn != null)
                {
                    foreach (string line in returnSetup) body.Append(Indent(line, "            "));
                    string returned = Unique("__managedResult");
                    body.AppendLine("            var " + returned + " = " + convertedReturn.Value.Expression + ";"); body.AppendLine("            " + convertedReturn.Value.Complete);
                    body.AppendLine("            return " + returned + ";");
                }
                else body.AppendLine($"            return {(returnType == "bool" ? value + " != 0" : value)};");
            }
            body.AppendLine("        }");
            if (hasBodies)
            {
                string final = "";
                foreach (string action in cleanup) final = final.Length == 0 ? action : "try {\n" + action + "\n} finally {\n" + final + "\n}";
                body.AppendLine("        }\n        finally\n        {");
                if (final.Length != 0) body.Append(Indent(final, "            "));
                body.AppendLine("        }");
            }
            if (scriptingLifetime)
            {
                body.AppendLine("        }\n        finally\n        {");
                foreach (string line in scriptingKeepAlive) body.AppendLine("            " + line);
                if (scriptingReceiver) body.AppendLine("            global::System.GC.KeepAlive(this);");
                body.AppendLine("        }");
            }
            if (instance && !structure && !scriptingReceiver)
                body.AppendLine($"        }}\n        finally {{ if ({lease}) __NativeLifetime.DangerousRelease(); }}");
            if (constructor)
                body.AppendLine($"        {constructorTransferred} = true;\n        return {constructorOwner};\n        }}\n        finally {{ if (!{constructorTransferred}) {{ if ({constructorOwner} != null) {constructorOwner}.Dispose(); else if ({value} != 0) {release}({value}); }} }}");
            body.AppendLine("    }\n}");
            if (constructor)
            {
                declarations.Append(Indent(body.ToString(), "    "));
                string forwarded = string.Join(", ", entry.Parameters!.Select(p =>
                    (p.Passing == ParameterPassing.LValueReference ? p.Direction switch { ParameterDirection.In => "in ", ParameterDirection.Out => "out ", _ => "ref " } : "") + p.ManagedName));
                body.Clear(); body.AppendLine($"{entry.ManagedDeclaration} : this(global::{imports}.Create{importName}({forwarded})) {{ }}");
            }
            string indentation = new(managed.Source[entry.ManagedSourceStart..].TakeWhile(c => c == ' ').ToArray());
            edits.Add((entry.ManagedSourceStart, entry.ManagedSourceLength, Indent(body.ToString(), indentation)));
        }
        if (!result.Success) return result;
        // Existing emitted owners (including nested structs) receive their helper
        // in the actual body. Splitting a qualified type name at its last dot
        // would mistake an enclosing class for a namespace.
        var embeddedOwners = new HashSet<string>(StringComparer.Ordinal);
        foreach (var (support, owner) in converterSupport.Keys)
        {
            if (owner == null) continue;
            string container = owner.Replace("global::", "");
            var ownerBinding = typeBindings.Values.FirstOrDefault(binding =>
                HandleNetCS.ManagedName(binding.Native).Replace("global::", "") == container);
            if (ownerBinding == null || ownerBinding.ManagedSourceStart < 0) continue;
            string name = SupportName(support);
            string body = support.Replace("global::{Support}", "global::" + container + "." + name).Replace("{Support}", name);
            edits.Add((ownerBinding.ManagedSourceStart, 0, "\n" + body + "\n"));
            embeddedOwners.Add(container);
        }
        StringBuilder source = new(managed.Source);
        foreach (var edit in edits.OrderByDescending(e => e.Start)) source.Remove(edit.Start, edit.Length).Insert(edit.Start, edit.Text);
        source.AppendLine($"\ninternal static unsafe class {imports}\n{{");
        source.Append(declarations); source.AppendLine("}");
        foreach (var (support, owner) in converterSupport.Keys)
        {
            string name = SupportName(support), container = owner?.Replace("global::", "") ?? "Interop";
            if (owner != null && embeddedOwners.Contains(container)) continue;
            string body = support.Replace("global::{Support}", "global::" + container + "." + name).Replace("{Support}", name);
            if (owner == null) source.AppendLine("namespace Interop {\n" + body + "\n}");
            else if (converterSupport[(support, owner)].WrapManagedSupport is { } wrap) source.AppendLine(wrap(body));
            else
            {
                int split = container.LastIndexOf('.');
                string type = split < 0 ? container : container[(split + 1)..];
                if (split >= 0) source.AppendLine("namespace " + container[..split] + " {");
                source.AppendLine("public partial struct " + type + " {\n" + body + "\n}");
                if (split >= 0) source.AppendLine("}");
            }
        }
        if (handles.Count != 0)
        {
            source.AppendLine($$"""
                #nullable enable annotations
                internal sealed class {{lifetime}} : global::System.Runtime.InteropServices.SafeHandle
                {
                    private readonly global::System.Action<nint>? release;
                    internal {{lifetime}}(nint address, global::System.Action<nint>? release) : base(0, true) { SetHandle(address); this.release = release; }
                    public override bool IsInvalid => handle == 0;
                    protected override bool ReleaseHandle() { try { release?.Invoke(handle); return true; } catch { return false; } }
                }
                #nullable disable annotations
                """);
        }
        result.Source = source.ToString().Replace("// Declaration preview only: no native calls, marshalling or ABI layout guarantees.", "// Native imports wired from the emitted binding manifest; ABI: Windows x64.")
            .Replace("// Method bodies throw; C++ initializers and function implementations are not translated.", "// Native calls execute through generated wrappers; C++ initializers are not translated.");
        if (native.Events.Count != 0) result.Source += EventInterop.Runtime(suffix);
        return result;
        void Error(EmittedBinding binding, string message) => result.Diagnostics.Add(new LogEntry(message, binding.Native.Name, LogEntryType.Error, "VETCS003"));
        bool EnsureNativeView(TypeDescriptor type, EmittedBinding origin)
        {
            if (views.Contains(type)) return true;
            if (entries.Any(e => e.Native.Parent == type && e.Native.Name.text is "FromNative" or "__NativeAddress"))
            { Error(origin, "Generated native view member conflicts with FromNative or __NativeAddress."); return false; }
            var binding = typeBindings[type];
            string name = binding.ManagedDeclaration;
            string helper = "// Borrowed view only: copying this struct aliases the same native storage.\n" +
                "private readonly nint __NativeAddress;\n" +
                $"private {name}(nint address) {{ __NativeAddress = address; }}\n" +
                "/// <summary>Creates a borrowed native view; returns null for zero. The caller must keep the native storage alive.</summary>\n" +
                $"public static {name}? FromNative(nint address) => address != 0 ? new {name}(address) : ({name}?)null;\n";
            int previousLineEnd = binding.ManagedSourceStart - 1;
            while (previousLineEnd >= 0 && managed.Source[previousLineEnd] is '\r' or '\n') --previousLineEnd;
            int previousLineStart = managed.Source.LastIndexOf('\n', previousLineEnd) + 1;
            int parentIndent = managed.Source[previousLineStart..(previousLineEnd + 1)].TakeWhile(c => c == ' ').Count();
            edits.Add((binding.ManagedSourceStart, 0, Indent(helper, new string(' ', parentIndent + 4))));
            views.Add(type);
            return true;
        }
        bool EnsureHandle(ClassDescriptor cls, EmittedBinding origin)
        {
            if (handles.Contains(cls)) return true;
            if (resolvingHandles.Count >= 64 || !resolvingHandles.Add(cls))
            { Error(origin, "Native lifetime inheritance is cyclic or exceeds the supported depth: " + cls.QualifiedName); return false; }
            try { return BuildHandle(cls, origin); }
            finally { resolvingHandles.Remove(cls); }
        }
        bool BuildHandle(ClassDescriptor cls, EmittedBinding origin)
        {
            if (entries.Any(e => e.Native.Parent == cls && e.Native.Name.text is "FromNative" or "__NativeAddress" or "__NativeLifetime" or "Dispose" or "__BorrowedNative"))
            { Error(origin, "Generated lifetime member conflicts with FromNative, Dispose, __NativeAddress or __NativeLifetime."); return false; }
            if (!typeBindings.TryGetValue(cls, out var typeBinding))
            {
                if (managed.ProjectClassLifetimes.Contains(cls)) return true;
                Error(origin, $"Resolved external base class '{cls.QualifiedName}', but its managed lifetime wrapper is not part of this emission unit. Cross-file lifetime coordination is required before emitting this native binding."); return false;
            }
            string baseInitializer = "";
            if (typeBinding.ClassBase is { } baseClass)
            {
                if (!EnsureHandle(baseClass, origin)) return false;
                if (!native.Upcasts.TryGetValue(typeBinding, out string? export))
                { Error(origin, "Missing native receiver upcast for " + cls.QualifiedName); return false; }
                string adjust = "Adjust" + index++;
                declarations.AppendLine($"    [global::System.Runtime.InteropServices.DllImport({Literal(libraryName)}, EntryPoint = {Literal(export)}, ExactSpelling = true, CallingConvention = global::System.Runtime.InteropServices.CallingConvention.Cdecl)]");
                declarations.AppendLine($"    private static extern int {adjust}Native(nint address, nint* result);");
                declarations.AppendLine($"    internal static nint {adjust}(nint address) {{ nint result = 0; int status = {adjust}Native(address, &result); if (status != 0 || result == 0) throw new global::System.InvalidOperationException(\"Native receiver upcast failed with status \" + status); return result; }}");
                baseInitializer = $" : base(global::{imports}.{adjust}(address), nativeLifetime)";
            }
            string name = typeBinding.ManagedDeclaration;
            string rootLifetime = typeBinding.ClassBase == null ?
                "protected readonly global::System.Runtime.InteropServices.SafeHandle __NativeLifetime;\n" +
                "/// <summary>Releases owned native storage or invalidates this borrowed wrapper. Safe to call more than once.</summary>\npublic void Dispose() => __NativeLifetime.Dispose();\n" : "";
            string borrowedType = cls.Api.Abstract ? "__BorrowedNative" : name;
            string borrowedProxy = cls.Api.Abstract ? $"private sealed class __BorrowedNative : {name} {{ internal __BorrowedNative({lifetime} nativeLifetime) : base(nativeLifetime) {{ }} }}\n" : "";
            string helper = rootLifetime + $"private readonly nint __NativeAddress;\n{(cls.IsFinal || cls.Api.Sealed ? "private" : "protected")} {name}(nint address, global::System.Runtime.InteropServices.SafeHandle nativeLifetime){baseInitializer} {{ __NativeAddress = address; {(typeBinding.ClassBase == null ? "__NativeLifetime = nativeLifetime;" : "")} }}\n" +
                $"private {name}({lifetime} nativeLifetime) : this(nativeLifetime.DangerousGetHandle(), nativeLifetime) {{ }}\n" +
                borrowedProxy +
                "#nullable enable annotations\n" +
                "/// <summary>Wraps a borrowed native address; returns null for zero. Does not allocate or free native storage.</summary>\n" +
                $"public {(typeBinding.ClassBase != null ? "new " : "")}static {name}? FromNative(nint address) => address != 0 ? new {borrowedType}(new {lifetime}(address, null)) : null;\n" +
                "#nullable disable annotations\n";
            int previousLineEnd = typeBinding.ManagedSourceStart - 1;
            while (previousLineEnd >= 0 && managed.Source[previousLineEnd] is '\r' or '\n') --previousLineEnd;
            int previousLineStart = managed.Source.LastIndexOf('\n', previousLineEnd) + 1;
            int parentIndent = managed.Source[previousLineStart..(previousLineEnd + 1)].TakeWhile(c => c == ' ').Count();
            edits.Add((typeBinding.ManagedSourceStart, 0, Indent(helper, new string(' ', parentIndent + 4))));
            if (typeBinding.ClassBase == null)
            {
                string typeIndent = new(managed.Source[typeBinding.ManagedTypeHeaderStart..].TakeWhile(c => c == ' ').ToArray());
                edits.Add((typeBinding.ManagedTypeHeaderStart, typeBinding.ManagedTypeHeaderLength,
                    Indent(typeBinding.ManagedTypeHeader + (cls.BaseTypes.Count == 0 ? " : " : ", ") + "global::System.IDisposable", typeIndent)));
            }
            handles.Add(cls);
            return true;
        }
        string ReleaseMethod(ClassDescriptor cls, EmittedBinding origin)
        {
            if (releaseMethods.TryGetValue(cls, out string? existing)) return existing;
            if (!native.Releases.TryGetValue(typeBindings[cls], out string? export))
            { Error(origin, "Missing native delete function for owned class " + cls.QualifiedName); return ""; }
            string method = "Release" + index++;
            declarations.AppendLine($"    [global::System.Runtime.InteropServices.DllImport({Literal(libraryName)}, EntryPoint = {Literal(export)}, ExactSpelling = true, CallingConvention = global::System.Runtime.InteropServices.CallingConvention.Cdecl)]");
            declarations.AppendLine($"    private static extern int {method}Native(nint address);");
            declarations.AppendLine($"    internal static void {method}(nint address) {{ int status = {method}Native(address); if (status != 0) throw new global::System.InvalidOperationException(\"Native deletion failed with status \" + status); }}");
            releaseMethods.Add(cls, method); return method;
        }
    }

    static string ParameterType(string type) => type.StartsWith("ref readonly ") ? type[13..] : type.StartsWith("ref ") || type.StartsWith("out ") ? type[4..] : type.StartsWith("in ") ? type[3..] : type;
    static ConverterBodies? Bodies(VETools.Text.TypeInfo? type) => type is ReferenceTypeInfo reference ? Bodies(reference.Referent) : (type as NamedTypeInfo)?.Converter?.Bodies;
    static string Indent(string text, string indentation) => string.Join("\n", text.TrimEnd('\r', '\n').Replace("\r", "").Split('\n').Select(line => indentation + line)) + "\n";
    static string Literal(string text) => "\"" + text.Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\r", "\\r").Replace("\n", "\\n").Replace("\0", "\\0") + "\"";
}

