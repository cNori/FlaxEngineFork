using System.Security.Cryptography;
using System.Text;
using VETools.Text;
using TypeInfo = VETools.Text.TypeInfo;

namespace VETools.Generation;

internal static class EventInterop
{
    internal sealed record Argument(string Native, string Managed, string WireCpp, string WireCS, ConverterBodies? InlineBody = null, TypeInfo? ObjectType = null, NativeTypeConverter? CopiedConverter = null, bool MutableReference = false);
    internal sealed record Description(IReadOnlyList<Argument> Arguments, string? SynchronousDelegate = null, string NativeFailure = "", NativeSupportRequirements? NativeRequirements = null)
    {
        public bool Synchronous => SynchronousDelegate != null;
        public string ActionType => SynchronousDelegate ?? "global::System.Action" + (Arguments.Count == 0 ? "" : "<" + string.Join(", ", Arguments.Select(a => a.Managed)) + ">");
    }
    public static bool Describe(FieldDescriptor field, out Description? description, out string reason)
    {
        description = null; reason = "";
        if (InstanceEventInterop.IsInstance(field) && !InstanceEventInterop.Supported(field))
        { reason = "instance events require an explicit native-owner destruction/unsubscription hook; that lifecycle bridge is not implemented yet."; return false; }
        if (field.Access != "public" && (field.Parent is not TypeDescriptor accessOwner || !CppGenerator.HasFriendAccess(accessOwner) || !InstanceEventInterop.UsesLifetime(field))) { reason = "native event is not public; a native access adapter is required."; return false; }
        if (field.TypeInfo is not NamedTypeInfo named || (named.Symbol?.QualifiedName ?? named.Name.TrimStart(':')) != "Delegate")
        { reason = "expected a native Delegate<T...> event."; return false; }
        if (named.EffectiveQualifiers != TypeQualifiers.None) { reason = "a const/volatile native event source requires an explicit subscription adapter."; return false; }
        if (named.Arguments.Count > 16) { reason = "more than 16 arguments require a custom delegate handler."; return false; }
        bool synchronous = field.SourceFile?.Converters.IsSynchronousEvent(field.QualifiedName) == true;
        if (synchronous && !InstanceEventInterop.UsesLifetime(field))
        { reason = "synchronous delivery requires the host Delegate lifetime bridge."; return false; }
        List<Argument> args = [];
        for (int argumentIndex = 0; argumentIndex < named.Arguments.Count; ++argumentIndex)
        {
            var parameter = named.Arguments[argumentIndex];
            if (field.SourceFile?.Converters.IsBorrowedEventPointer(field.QualifiedName, argumentIndex) == true)
            {
                if (!synchronous || parameter.Type is not PointerTypeInfo { Qualifiers: TypeQualifiers.None, Pointee: NamedTypeInfo { Name: "void", Resolution: TypeResolution.Builtin, EffectiveQualifiers: TypeQualifiers.None } })
                { reason = "configured borrowed event pointer requires synchronous delivery and a single unqualified void* argument."; return false; }
                args.Add(new("void*", "nint", "void*", "nint"));
                continue;
            }
            if (parameter.Type is PointerTypeInfo { Qualifiers: TypeQualifiers.None, Pointee: NamedTypeInfo { EffectiveQualifiers: TypeQualifiers.None } } pointer &&
                HandleNETcpp.Use(pointer) is { Pointer: true, Slot: false, Reference: false } scripting && HandleNETcpp.CanCast(scripting.Type))
            {
                args.Add(new(TemplateInstantiationPass.Native(pointer), HandleNetCS.ObjectType(pointer)!, "void*", "nint", ObjectType: pointer));
                continue;
            }
            TypeInfo? payloadType = parameter.Type;
            bool mutableReference = false;
            if (payloadType is ReferenceTypeInfo reference)
            {
                if (reference.ReferenceKind != ReferenceKind.LValue || reference.Referent is not NamedTypeInfo referenced || (referenced.EffectiveQualifiers & TypeQualifiers.Volatile) != 0)
                { reason = $"payload '{payloadType.Display}' requires an explicit reference/lifetime adapter."; return false; }
                mutableReference = (referenced.EffectiveQualifiers & TypeQualifiers.Const) == 0;
                if (mutableReference && !synchronous)
                { reason = $"payload '{payloadType.Display}' is a writable reference. Queued events cannot copy changes back into the completed native call; register a same-thread synchronous event contract."; return false; }
                payloadType = reference.Referent;
            }
            if (payloadType is not NamedTypeInfo type)
            { reason = $"payload '{payloadType?.Display}' contains a raw pointer or unresolved type. Supply a copied-value/lifetime converter; queued delivery cannot retain borrowed storage."; return false; }
            if ((type.EffectiveQualifiers & TypeQualifiers.Volatile) != 0)
            { reason = $"volatile payload '{type.Display}' requires an explicit synchronization adapter."; return false; }
            string nativeType = TemplateInstantiationPass.Native(parameter.Type!);
            if (type.Converter is { Bodies: { } copiedBody } copied &&
                (copiedBody.CopiedCallbackValue || synchronous && mutableReference && copied.AutomaticFields && copiedBody.CopyReferenceParameters))
            {
                args.Add(new(nativeType, copied.Bodies!.ManagedType, copied.AbiCppType, copied.Bodies.ManagedAbiType, CopiedConverter: copied, MutableReference: mutableReference));
                continue;
            }
            ScalarAbi? scalar = ScalarAbi.Resolve(type);
            string? managed = scalar?.CS;
            ConverterBodies? inline = null;
            if (type.Converter is { EnumValue: true, Bodies: { } body } enumeration)
            { scalar = ScalarAbi.Require(enumeration); managed = body.ManagedType; }
            else if (type.Converter is { Bodies.InlineCallbackValue: true } value)
            {
                inline = value.Bodies!;
                bool Simple(ConverterBody conversion) => conversion.Expression.Length != 0 && conversion.Declarations.Length == 0 && conversion.Setup.Length == 0 && conversion.Cleanup.Length == 0;
                var context = new ConverterEmitContext("value", null, name => name, "false", "false");
                if (inline.ManagedSupport.Length != 0 || inline.NativeSupport.Length != 0 || value.ManagedDeclarations.Count != 0 ||
                    inline.NativeRequirements.Includes.Count != 0 || inline.NativeRequirements.Declarations.Count != 0 || inline.NativeRequirements.Libraries.Count != 0 ||
                    !Simple(inline.NativeToAbi(context)) || !Simple(inline.AbiToManaged(context)))
                { reason = $"inline callback converter for '{type.Display}' requires helpers, temporary storage or cleanup; this needs an owning payload bridge."; return false; }
                scalar = ScalarAbi.Require(value); managed = inline.ManagedType;
            }
            if (scalar == null)
            { reason = $"payload '{type.Display}' needs an approved inline value converter or an owning payload bridge; only scalar, enum and explicitly self-contained value conversions are supported."; return false; }
            if (mutableReference && inline != null)
            { reason = "mutable custom inline values need an explicit synchronous reverse-conversion contract."; return false; }
            args.Add(new(nativeType, managed!, scalar.CS == "bool" ? "unsigned char" : scalar.Cpp, scalar.CS switch { "bool" => "byte", "char" => "ushort", _ => scalar.CS }, inline, MutableReference: mutableReference));
        }
        foreach (string name in new[] { "Pump" + field.Name.text + "Events", field.Name.text + "DispatchError", "Shutdown" + field.Name.text + "Events" })
            if (field.Parent is ScopeDescriptor scope && scope.Declarations.Any(d => d != field && d.Name.text == name))
            { reason = "generated event control member conflicts with native member '" + name + "'."; return false; }
        string? handler = synchronous ? field.Name.text + "Handler" : null;
        if (handler != null && field.Parent is ScopeDescriptor container && container.Declarations.Any(d => d.Name.text == handler))
        { reason = "generated synchronous delegate name conflicts with native member '" + handler + "'."; return false; }
        description = new(args, handler, synchronous ? field.SourceFile?.Converters.SynchronousEventFailure(field.QualifiedName) ?? "" : "", field.SourceFile?.Converters.SynchronousEventRequirements(field.QualifiedName)); return true;
    }

    public static IEnumerable<NativeSupportRequirements> Requirements(IEnumerable<EmittedBinding> entries) => entries
        .Where(e => e.Kind == EmittedBindingKind.Event).Select(e => Describe((FieldDescriptor)e.Native, out var description, out _) ? description?.NativeRequirements ?? new() : new NativeSupportRequirements());

    public static IEnumerable<NativeTypeConverter> CopiedConverters(IEnumerable<EmittedBinding> entries) => entries
        .Where(e => e.Kind == EmittedBindingKind.Event).SelectMany(e =>
            Describe((FieldDescriptor)e.Native, out var description, out _) ? description!.Arguments.Select(a => a.CopiedConverter).OfType<NativeTypeConverter>() : []);

    static (string Setup, string Values, string Finish) NativePayload(Description description)
    {
        List<string> setup = [], values = [], decode = [], commit = [];
        int counter = 0;
        string Unique(string name) => "__event_" + name + "_" + counter++;
        for (int i = 0; i < description.Arguments.Count; ++i)
        {
            var argument = description.Arguments[i];
            string input = "arg" + i;
            if (argument.CopiedConverter?.Bodies is { } converter)
            {
                var body = converter.NativeToAbi(new(input, ParameterDirection.In, Unique, "false", "false", CppGenerator.SupportName(converter.NativeSupport)));
                setup.Add(body.Declarations);
                if (body.Cleanup.Length != 0) setup.Add("auto " + Unique("cleanup") + " = VETConverterFinally([&]() { " + body.Cleanup + " });");
                setup.Add(body.Setup);
                string value = Unique("payload"); setup.Add("auto " + value + " = " + body.Expression + ";"); values.Add(value);
            }
            else values.Add(argument.ObjectType is { } obj ? HandleNETcpp.ToRoot(HandleNETcpp.Use(obj)!.Type, input)
                : argument.InlineBody?.NativeToAbi(new(input, null, Unique, "false", "false")).Expression ?? "static_cast<" + argument.WireCpp + ">(" + input + ")");
            if (argument.MutableReference)
            {
                string output = Unique("output"); setup.Add(argument.WireCpp + " " + output + "{};"); values.Add("&" + output);
                if (argument.CopiedConverter?.Bodies is { } copied)
                {
                    if (copied.ReleaseNativeAbi == null) throw new InvalidOperationException("Synchronous callback packet needs native release support.");
                    setup.Add("auto " + Unique("outputCleanup") + " = VETConverterFinally([&]() { " + copied.ReleaseNativeAbi(new(output, ParameterDirection.Ref, Unique, "false", "false", CppGenerator.SupportName(copied.NativeSupport))) + " });");
                    var decoded = copied.AbiToNative(new(output, ParameterDirection.Ref, Unique, "false", "false", CppGenerator.SupportName(copied.NativeSupport)));
                    decode.Add(decoded.Declarations); decode.Add(decoded.Setup);
                    string local = Unique("decoded"); decode.Add("auto " + local + " = " + decoded.Expression + ";");
                    commit.Add(input + " = std::move(" + local + ");");
                }
                else commit.Add(input + " = static_cast<typename std::remove_reference<decltype(" + input + ")>::type>(" + output + ");");
            }
        }
        return (string.Join("\n", setup), string.Join(", ", values), string.Join("\n", decode.Concat(commit)));
    }

    static string ManagedPayload(Description description, Func<ConverterBodies, string> supportPath)
    {
        List<string> setup = []; int counter = 0;
        string Unique(string name) => "__event_" + name + "_" + counter++;
        for (int i = 0; i < description.Arguments.Count; ++i)
        {
            if (description.Arguments[i].CopiedConverter?.Bodies is not { } converter)
            {
                var argument = description.Arguments[i];
                if (argument.MutableReference) setup.Add("var __eventValue" + i + " = " + (argument.Managed == "bool" ? "arg" + i + " != 0" : "(" + argument.Managed + ")arg" + i) + ";");
                continue;
            }
            var body = converter.AbiToManaged(new("arg" + i, ParameterDirection.In, Unique, "false", "false", supportPath(converter)));
            // Callback packets remain native-owned. Decode now; only the independent managed value enters the queue.
            setup.Add(body.Declarations); setup.Add(body.Setup); setup.Add("var __eventValue" + i + " = " + body.Expression + ";");
        }
        return string.Join("\n", setup);
    }

    static string Hash(string value) => Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(value)))[..16];
    public static IEnumerable<string> DefinitionHeaders(EmittedBinding entry)
    {
        if (entry.Native is not FieldDescriptor field || !Describe(field, out var description, out _)) yield break;
        foreach (var argument in description!.Arguments)
        {
            if (HandleNETcpp.Use(argument.ObjectType)?.Type.SourceFile?.Path is { } path) yield return path;
            foreach (string dependency in argument.CopiedConverter?.DefinitionHeaders ?? []) yield return dependency;
        }
    }
    internal static string Accessor(FieldDescriptor field) => "Event_" + Hash(field.NativeQualifiedName);
    internal static string AccessPointer(FieldDescriptor field) => $"::VETBindingAccess<::{field.Parent!.NativeQualifiedName}>::{Accessor(field)}()";

    public static void EmitNative(StringBuilder code, IReadOnlyList<EmittedBinding> entries, CppGenerationResult result, string header, NativeEntryPointNaming naming, string? version)
    {
        foreach (var entry in entries.Where(e => e.Kind == EmittedBindingKind.Event))
        {
            var field = (FieldDescriptor)entry.Native;
            if (!Describe(field, out var description, out _)) throw new InvalidOperationException("Invalid event survived managed emission.");
            var args = description!.Arguments;
            string digest = Hash((description.Synchronous ? "sync-v1|" + description.NativeFailure + "|" : "") + Path.GetFullPath(header) + (InstanceEventInterop.UsesLifetime(field) ? "|event-lifetime-v1|" : "|event|") + (args.Any(a => a.CopiedConverter != null) ? "snapshot-v1|" : "") + field.NativeQualifiedName + "|" + (naming == NativeEntryPointNaming.Protected ? version : ""));
            string bridge = "VETEventBridge_" + digest;
            string export = naming == NativeEntryPointNaming.Readable ? "CppGlue_" + System.Text.RegularExpressions.Regex.Replace(field.QualifiedName, "[^A-Za-z0-9_]", "_") + "_Internal_Bind_" + digest : "VET_" + digest;
            string parameters = string.Join(", ", args.Select((a, i) => a.Native + " arg" + i));
            var payload = NativePayload(description);
            string values = payload.Values;
            string target = "::" + field.NativeQualifiedName.TrimStart(':');
            if (InstanceEventInterop.UsesLifetime(field))
            {
                InstanceEventInterop.EmitNative(code, field, description, bridge, export, parameters, values, payload.Setup, payload.Finish);
                result.Events.Add(entry, export);
                continue;
            }
            code.AppendLine($$"""
                namespace {{bridge}} {
                using Callback = void(CallDeclaraton*)({{string.Join(", ", args.Select(a => a.WireCpp))}});
                struct State { std::recursive_mutex Gate; std::mutex Operation; Callback Target = nullptr; };
                // Keep native-only hook state alive until module unload, avoiding static destruction-order races.
                inline State& Get() { static State* state = new State(); return *state; }
                static void Invoke({{parameters}}) noexcept {
                    try { auto& state = Get(); std::lock_guard<std::recursive_mutex> lock(state.Gate); if (state.Target) { {{payload.Setup}} state.Target({{values}}); } } catch (...) { }
                }
                }
                extern "C" DLLExport int CallDeclaraton {{export}}(void* callback, int enable) noexcept {
                    if (!callback || (enable != 0 && enable != 1)) return 1;
                    try {
                        auto& state = {{bridge}}::Get(); std::lock_guard<std::mutex> operation(state.Operation);
                        auto target = reinterpret_cast<{{bridge}}::Callback>(callback);
                        if (enable) {
                            { std::lock_guard<std::recursive_mutex> lock(state.Gate); if (state.Target && state.Target != target) return 3; }
                            // Idempotent native hook; never holds Gate while taking the Delegate's lock.
                            {{target}}.template Bind<&{{bridge}}::Invoke>();
                            std::lock_guard<std::recursive_mutex> lock(state.Gate); state.Target = target;
                        } else {
                            { std::lock_guard<std::recursive_mutex> lock(state.Gate); if (state.Target && state.Target != target) return 3; }
                            {{target}}.template Unbind<&{{bridge}}::Invoke>();
                            std::lock_guard<std::recursive_mutex> lock(state.Gate);
                            state.Target = nullptr; // Waited for in-flight reverse calls to return before releasing the managed root.
                        }
                        return 0;
                    } catch (...) { return 2; }
                }
                """);
            result.Events.Add(entry, export);
        }
    }

    public static string EmitManaged(EmittedBinding entry, string export, string library, string suffix, Func<ConverterBodies, string> supportPath)
    {
        var field = (FieldDescriptor)entry.Native; Describe(field, out var description, out _);
        var args = description!.Arguments;
        string key = Hash(field.QualifiedName), stem = "__event_" + key, runtime = "global::Interop.__EventRuntime_" + suffix;
        string name = CSharpGenerator.Qualified(field.Name.text), action = description.ActionType;
        string parameters = string.Join(", ", args.Select((a, i) => a.WireCS + " arg" + i + (a.MutableReference ? ", " + a.WireCS + "* output" + i : "")));
        string values = string.Join(", ", args.Select((a, i) => a.MutableReference ? "ref __eventValue" + i : a.CopiedConverter != null ? "__eventValue" + i : a.ObjectType != null ? "__eventObject" + i : a.InlineBody?.AbiToManaged(new("arg" + i, null, n => n, "false", "false")).Expression ?? (a.Managed == "bool" ? "arg" + i + " != 0" : "(" + a.Managed + ")arg" + i)));
        // Resolve borrowed pointers while the native callback's arguments are
        // valid. Only managed engine identities may enter the deferred closure.
        string objects = string.Join("\n", args.Select((a, i) => a.ObjectType is { } obj ? "var __eventObject" + i + " = " + HandleNetCS.FromNative(obj, "arg" + i) + ";" : ""));
        objects += "\n" + ManagedPayload(description, supportPath);
        string validation = string.Join("\n", args.Select((a, i) => a.InlineBody == null && a.Managed == "bool" ? "if (arg" + i + " > 1) throw new global::System.InvalidOperationException(\"Invalid event bool.\");" : ""));
        if (InstanceEventInterop.UsesLifetime(field)) return InstanceEventInterop.EmitManaged(entry, export, library, suffix, description, stem, parameters, values, validation, objects, supportPath);
        return $$"""
            {{entry.ManagedDeclaration}} { add => {{stem}}.Add(value); remove => {{stem}}.Remove(value); }
            /// <summary>Runs pending event handlers on the subscription owner thread. Call from the owner loop when no dispatcher context is available.</summary>
            public static int Pump{{field.Name.text}}Events(int limit = 1024) => {{stem}}.Pump(limit);
            /// <summary>Most recent dispatch, queue overflow, context, or subscriber error; no exception crosses the native callback boundary.</summary>
            public static global::System.Exception {{name}}DispatchError => {{stem}}.Error;
            /// <summary>Permanently cancels this event bridge and waits for native callbacks to return. Run before unloading native modules.</summary>
            public static void Shutdown{{field.Name.text}}Events() => {{stem}}.Shutdown();
            [global::System.Runtime.InteropServices.UnmanagedFunctionPointer(global::System.Runtime.InteropServices.CallingConvention.Cdecl)]
            private delegate void {{stem}}Callback({{parameters}});
            private static readonly {{stem}}Callback {{stem}}Root = {{stem}}Receive;
            private static readonly {{runtime}}.Slot<{{action}}> {{stem}} = new(
                () => {{stem}}Bind(true), () => {{stem}}Bind(false));
            private static void {{stem}}Bind(bool enable) {
                int status = {{stem}}NativeBind(global::System.Runtime.InteropServices.Marshal.GetFunctionPointerForDelegate({{stem}}Root), enable ? 1 : 0);
                if (status != 0) throw new global::System.InvalidOperationException("Native event binding failed with status " + status);
                global::System.GC.KeepAlive({{stem}}Root);
            }
            private static void {{stem}}Receive({{parameters}}) {
                try { {{validation}}
                    {{objects}}
                    {{stem}}.Emit(handler => handler({{values}}));
                } catch (global::System.Exception error) { {{stem}}.Report(error); }
            }
            [global::System.Runtime.InteropServices.DllImport({{System.Text.Json.JsonSerializer.Serialize(library)}}, EntryPoint = {{System.Text.Json.JsonSerializer.Serialize(export)}}, ExactSpelling = true, CallingConvention = global::System.Runtime.InteropServices.CallingConvention.Cdecl)]
            private static extern int {{stem}}NativeBind(nint callback, int enable);
            """;
    }

    internal static string SynchronousReceive(Description description, string stem, string parameters, string values, string validation, string objects,
        Func<ConverterBodies, string> supportPath)
    {
        List<string> declarations = [], setup = [], cleanup = [], commit = []; int counter = 0;
        string Unique(string name) => "__sync_" + name + "_" + counter++;
        for (int i = 0; i < description.Arguments.Count; ++i)
        {
            var argument = description.Arguments[i]; if (!argument.MutableReference) continue;
            validation += "\nif (output" + i + " == null) throw new global::System.ArgumentNullException(\"output" + i + "\");";
            string value = "__eventValue" + i;
            if (argument.CopiedConverter?.Bodies is { } converter)
            {
                string complete = Unique("converted"); declarations.Add("bool " + complete + " = false;");
                var body = converter.ManagedToAbi(new(value, ParameterDirection.Ref, Unique, complete, "false", supportPath(converter)));
                declarations.Add(body.Declarations); setup.Add(body.Setup);
                string result = Unique("wire"); setup.Add(argument.WireCS + " " + result + " = " + body.Expression + "; " + complete + " = true; _ = " + complete + ";");
                cleanup.Add("if (!__committed) { " + body.Cleanup + " }"); commit.Add("*output" + i + " = " + result + ";");
            }
            else commit.Add("*output" + i + " = " + (argument.Managed == "bool" ? "(byte)(" + value + " ? 1 : 0)" : "(" + argument.WireCS + ")" + value) + ";");
        }
        string final = "";
        foreach (string action in cleanup) final = final.Length == 0 ? action : "try { " + action + " } finally { " + final + " }";
        return "private static unsafe int " + stem + "Receive(ulong cookie" + (parameters.Length == 0 ? "" : ", " + parameters) + ") {\n" +
            "if (!" + stem + "Roots.TryGetValue(cookie, out var state) || !state.Slot.AcceptsEvents) return 1;\n" +
            "bool __committed = false; " + string.Join("\n", declarations) + "\ntry {\nstate.Slot.RequireSynchronousOwner();\n" + validation + "\n" + objects +
            "\nif (!state.Slot.EmitSynchronous(handler => handler(" + values + "), " + (description.NativeFailure.Length != 0 ? "true" : "false") + ")) return 1;\n" + string.Join("\n", setup) + "\n" + string.Join("\n", commit) +
            "\n__committed = true; _ = __committed; return 0;\n} catch (global::System.Exception error) { state.Slot.Report(error); return 2; } finally { " + final + " }\n}";
    }

    public static string Runtime(string suffix) => RuntimeSource.Replace("{Runtime}", "__EventRuntime_" + suffix);

    const string RuntimeSource = """
        namespace Interop {
        internal static class {Runtime} {
            internal sealed class Slot<T> where T : global::System.Delegate {
                readonly object gate = new(), operation = new();
                readonly global::System.Action bind, unbind;
                readonly global::System.Collections.Generic.Queue<(long Version, T Snapshot, global::System.Action<T> Invoke)> queue = new();
                global::System.Threading.Thread owner;
                global::System.Threading.SynchronizationContext context;
                T handlers;
                long version; bool disposed, draining, nativeCancelled, bound; int posted, delivering;
                internal bool NativeCancelled { get { lock (gate) return nativeCancelled; } }
                internal bool AcceptsEvents { get { lock (gate) return !disposed && handlers != null; } }
                global::System.Exception error;
                internal Slot(global::System.Action bind, global::System.Action unbind, bool registerUnload = true) {
                    this.bind = bind; this.unbind = unbind;
                    var load = global::System.Runtime.Loader.AssemblyLoadContext.GetLoadContext(typeof(Slot<T>).Assembly);
                    if (registerUnload && load != null) load.Unloading += _ => { try { Dispose(); } catch (global::System.Exception e) { Report(e); } };
                }
                internal global::System.Exception Error { get { lock (gate) return error; } }
                internal void Report(global::System.Exception e) { lock (gate) error = e; }
                void RequireOwner(bool capture, bool allowDisposed = false) {
                    lock (gate) {
                        if (disposed && !allowDisposed) throw new global::System.ObjectDisposedException("Native event bridge");
                        if (owner == null && capture) { owner = global::System.Threading.Thread.CurrentThread; context = global::System.Threading.SynchronizationContext.Current; }
                        if (owner != null && owner != global::System.Threading.Thread.CurrentThread) throw new global::System.InvalidOperationException("Event subscription and pumping belong to the captured owner thread.");
                    }
                }
                internal void RequireSynchronousOwner() => RequireOwner(false);
                internal bool EmitSynchronous(global::System.Action<T> invoke, bool failOnHandlerError = false) {
                    RequireOwner(false); T snapshot; long captured; bool succeeded = true;
                    lock (gate) { if (disposed || handlers == null) return false; snapshot = handlers; captured = version; }
                    foreach (var item in snapshot.GetInvocationList()) {
                        lock (gate) { if (disposed || version != captured) { succeeded = false; break; } delivering++; }
                        try { invoke((T)item); } catch (global::System.Exception error) { succeeded = false; Report(error); }
                        finally { lock (gate) { delivering--; global::System.Threading.Monitor.PulseAll(gate); } }
                    }
                    return !failOnHandlerError || succeeded;
                }
                internal void Add(T handler) {
                    if (handler == null) return;
                    lock (operation) {
                        RequireOwner(true); bool first;
                        lock (gate) { first = handlers == null; handlers = (T)global::System.Delegate.Combine(handlers, handler); version++; queue.Clear(); }
                        if (first) try { bind(); lock (gate) { if (!disposed) bound = true; } } catch { lock (gate) { handlers = null; version++; queue.Clear(); } throw; }
                    }
                }
                internal void Remove(T handler) {
                    if (handler == null) return;
                    lock (operation) {
                        RequireOwner(false); bool last;
                        lock (gate) {
                            var remaining = (T)global::System.Delegate.Remove(handlers, handler);
                            if (global::System.Object.Equals(remaining, handlers) && (handlers != null || !bound)) return;
                            handlers = remaining; version++; queue.Clear(); last = handlers == null && bound;
                        }
                        if (last) { unbind(); lock (gate) bound = false; }
                    }
                }
                internal void Emit(global::System.Action<T> invoke) {
                    lock (gate) {
                        if (disposed || handlers == null) return;
                        if (queue.Count >= 1024) { error = new global::System.InvalidOperationException("Owner-thread event queue is full (1024); pump events more frequently."); return; }
                        queue.Enqueue((version, handlers, invoke));
                    }
                    Schedule();
                }
                void Schedule() {
                    global::System.Threading.SynchronizationContext dispatcher;
                    lock (gate) dispatcher = context;
                    if (dispatcher == null || dispatcher.GetType() == typeof(global::System.Threading.SynchronizationContext) || global::System.Threading.Interlocked.Exchange(ref posted, 1) != 0) return;
                    // Never execute user SynchronizationContext.Post under a native Delegate/callback lock.
                    global::System.Threading.ThreadPool.QueueUserWorkItem(_ => {
                        try { dispatcher.Post(_ => { global::System.Threading.Interlocked.Exchange(ref posted, 0); try { Pump(1024); } catch (global::System.Exception e) { Report(e); } }, null); }
                        catch (global::System.Exception e) { global::System.Threading.Interlocked.Exchange(ref posted, 0); Report(e); }
                    });
                }
                internal int Pump(int limit) {
                    RequireOwner(false);
                    if (limit < 1 || limit > 65536) throw new global::System.ArgumentOutOfRangeException(nameof(limit));
                    lock (gate) { if (draining) return 0; draining = true; }
                    int delivered = 0;
                    try {
                        for (int count = 0; count < limit; count++) {
                            (long Version, T Snapshot, global::System.Action<T> Invoke) work;
                            lock (gate) { if (disposed || queue.Count == 0) break; work = queue.Dequeue(); }
                            foreach (var item in work.Snapshot.GetInvocationList()) {
                                lock (gate) { if (disposed || version != work.Version) break; delivering++; }
                                try { work.Invoke((T)item); delivered++; } catch (global::System.Exception e) { Report(e); }
                                finally { lock (gate) { delivering--; global::System.Threading.Monitor.PulseAll(gate); } }
                            }
                        }
                    } finally { lock (gate) draining = false; }
                    return delivered;
                }
                internal void Shutdown() { RequireOwner(false, true); Dispose(); }
                internal void CancelFromNative() { lock (gate) { disposed = true; nativeCancelled = true; bound = false; handlers = null; version++; queue.Clear(); } }
                internal void Dispose() {
                    lock (operation) {
                        bool active;
                        lock (gate) { active = bound; disposed = true; handlers = null; version++; queue.Clear(); }
                        if (active) { unbind(); lock (gate) bound = false; }
                    }
                    // Foreign-thread unload waits for already-started managed deliveries.
                    // Release operation first so a handler attempting subscription changes can finish.
                    lock (gate) { while (delivering != 0 && owner != global::System.Threading.Thread.CurrentThread) global::System.Threading.Monitor.Wait(gate); }
                }
            }
        }
        }
        """;
}
