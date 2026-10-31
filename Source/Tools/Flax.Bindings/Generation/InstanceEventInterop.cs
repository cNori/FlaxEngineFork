using System.Text;
using VETools.Text;

namespace VETools.Generation;

internal static class InstanceEventInterop
{
    internal static bool IsInstance(FieldDescriptor field) => field.Parent is TypeDescriptor && !NativeLayout.IsStatic(field);
    internal static bool Supported(FieldDescriptor field) => field.Parent is ClassDescriptor owner &&
        owner.SourceFile?.Converters.FlaxDelegateLifetimes == true && HandleNETcpp.IsScriptingObject(owner) && HandleNETcpp.CanCast(owner);
    internal static bool UsesLifetime(FieldDescriptor field) => field.SourceFile?.Converters.FlaxDelegateLifetimes == true && (!IsInstance(field) || Supported(field));

    internal static void EmitNative(StringBuilder code, FieldDescriptor field, EventInterop.Description description, string bridge, string export, string parameters, string values, string payloadSetup, string payloadFinish)
    {
        bool instance = IsInstance(field);
        string native = instance ? "::" + field.Parent!.NativeQualifiedName : "";
        string root = instance ? HandleNETcpp.RootName((ClassDescriptor)field.Parent!) : "void";
        string delegateType = instance ? $"decltype(std::declval<{native}>().{field.Name.text})" : $"decltype(::{field.NativeQualifiedName})";
        string sourceExpression = instance ? $"&static_cast<{native}*>(owner)->{field.Name.text}" : "&::" + field.NativeQualifiedName;
        if (field.Access != "public")
        {
            string pointer = EventInterop.AccessPointer(field);
            sourceExpression = instance ? $"&(static_cast<{native}*>(owner)->*{pointer})" : pointer;
            string sourceValue = instance ? $"(std::declval<{native}>().*{pointer})" : $"*{pointer}";
            delegateType = $"typename std::remove_reference<decltype({sourceValue})>::type";
        }
        string wire = string.Concat(description.Arguments.Select(a => ", " + a.WireCpp + (a.MutableReference ? ", " + a.WireCpp + "*" : "")));
        string callbackReturn = description.Synchronous ? "int" : "void";
        string failure = description.NativeFailure.Length == 0 ? "" : "try { " + description.NativeFailure + " } catch (...) { }";
        string invoke = description.Synchronous ? "if (Target(Cookie" + (values.Length == 0 ? "" : ", " + values) + ") == 0) { " + payloadFinish + " } else { " + failure + " }" : "Target(Cookie" + (values.Length == 0 ? "" : ", " + values) + ");";
        code.AppendLine($$"""
            namespace {{bridge}} {
            using Callback = {{callbackReturn}}(CallDeclaraton*)(unsigned long long{{wire}});
            using Closed = void(CallDeclaraton*)(unsigned long long);
            using Source = {{delegateType}};
            inline int& CallbackDepth() { static thread_local int depth = 0; return depth; }
            struct CallbackScope { CallbackScope() { ++CallbackDepth(); } ~CallbackScope() { --CallbackDepth(); } };
            struct Hook;
            struct Context { Callback Target; Closed Notify; };
            struct Registry { std::mutex Operation, Gate; unsigned long long Next = 0; std::unordered_map<unsigned long long, Context> Contexts; std::unordered_map<unsigned long long, std::shared_ptr<Hook>> Hooks;
            #if defined(VET_EVENT_TEST_PROBES)
                std::atomic<bool> Busy{false};
            #endif
            };
            inline Registry& Get() { static Registry* registry = new Registry(); return *registry; }
            #if defined(VET_EVENT_TEST_PROBES)
            struct Probe { Registry& State; explicit Probe(Registry& state) : State(state) { State.Busy.store(true); } ~Probe() { State.Busy.store(false); } };
            #endif
            struct Hook : std::enable_shared_from_this<Hook> {
                std::recursive_mutex Gate;
                Callback Target = nullptr; Closed Notify = nullptr;
                unsigned long long ContextId = 0, Cookie = 0, Link = 0;
                std::shared_ptr<DelegateBindings::Lifetime> Lifetime;
                void Invoke({{parameters}}) noexcept {
                    try { auto hold = shared_from_this(); std::lock_guard<std::recursive_mutex> lock(Gate); if (Target) { {{payloadSetup}} CallbackScope callbackScope; {{invoke}} } } catch (...) { {{failure}} }
                }
                void Closing(void* source) noexcept {
                    auto hold = shared_from_this();
                    try { static_cast<Source*>(source)->template Unbind<Hook, &Hook::Invoke>(this); } catch (...) { }
                    { std::lock_guard<std::mutex> lock(Lifetime->CopyGate); Lifetime->NonCopyable.erase(this); }
                    { std::lock_guard<std::recursive_mutex> lock(Gate); Target = nullptr; auto notify = Notify; Notify = nullptr; if (notify) { try { notify(Cookie); } catch (...) { } } }
                    { auto& registry = Get(); std::lock_guard<std::mutex> lock(registry.Gate); auto i = registry.Hooks.find(Cookie); if (i != registry.Hooks.end() && i->second.get() == this) registry.Hooks.erase(i); }
                }
            };
            }
            extern "C" DLLExport int CallDeclaraton {{export}}_Context(void* callback, void* closed, unsigned long long* result) noexcept {
                if (!callback || !closed || !result) return 1;
                try { auto& registry = {{bridge}}::Get(); std::lock_guard<std::mutex> lock(registry.Gate);
                    if (registry.Next == ~0ULL) return 2;
                    auto id = ++registry.Next; registry.Contexts.emplace(id, {{bridge}}::Context{reinterpret_cast<{{bridge}}::Callback>(callback), reinterpret_cast<{{bridge}}::Closed>(closed)}); *result = id; return 0;
                } catch (...) { return 2; }
            }
            extern "C" DLLExport int CallDeclaraton {{export}}_Ticket(unsigned long long context, unsigned long long* result) noexcept {
                if (!context || !result) return 1;
                try { auto& registry = {{bridge}}::Get(); std::lock_guard<std::mutex> lock(registry.Gate);
                    if (registry.Contexts.find(context) == registry.Contexts.end()) return 5;
                    if (registry.Next == ~0ULL) return 2; *result = ++registry.Next; return 0;
                } catch (...) { return 2; }
            }
            extern "C" DLLExport int CallDeclaraton {{export}}({{root}}* owner, unsigned long long context, unsigned long long cookie, void* callback, void* closed, int enable) noexcept {
                {{(instance ? "" : "(void)owner;")}}
                if (!cookie || !callback || !closed || (enable != 0 && enable != 1){{(instance ? " || (enable && !owner)" : "")}}) return 1;
                if ({{bridge}}::CallbackDepth() != 0) return 4;
                try {
                    auto& registry = {{bridge}}::Get();
                    // Reverse-call conversion can reenter binding code. Report
                    // contention instead of waiting under a Delegate call lock.
                    std::unique_lock<std::mutex> operation(registry.Operation, std::try_to_lock);
                    if (!operation.owns_lock()) return 4;
            #if defined(VET_EVENT_TEST_PROBES)
                    {{bridge}}::Probe probe(registry);
            #endif
                    auto target = reinterpret_cast<{{bridge}}::Callback>(callback);
                    auto notify = reinterpret_cast<{{bridge}}::Closed>(closed);
                    std::shared_ptr<{{bridge}}::Hook> hook;
                    { std::lock_guard<std::mutex> lock(registry.Gate); auto i = registry.Hooks.find(cookie); if (i != registry.Hooks.end()) hook = i->second;
                        auto c = registry.Contexts.find(context); if (enable && (c == registry.Contexts.end() || c->second.Target != target || c->second.Notify != notify)) return 5; }
                    if (!enable) {
                        if (!hook) return 0;
                        if (hook->ContextId != context) return 3;
                        { std::lock_guard<std::recursive_mutex> lock(hook->Gate); if ((hook->Target && hook->Target != target) || (hook->Notify && hook->Notify != notify)) return 3; }
                        std::unique_lock<std::recursive_mutex> sourceLock(hook->Lifetime->Gate, std::try_to_lock);
                        if (!sourceLock.owns_lock()) return 4;
                        if (hook->Lifetime->Source) static_cast<{{bridge}}::Source*>(hook->Lifetime->Source)->template Unbind<{{bridge}}::Hook, &{{bridge}}::Hook::Invoke>(hook.get());
                        { std::lock_guard<std::mutex> lock(hook->Lifetime->CopyGate); hook->Lifetime->NonCopyable.erase(hook.get()); }
                        hook->Lifetime->Closing.erase(hook->Link);
                        { std::lock_guard<std::recursive_mutex> lock(hook->Gate); hook->Target = nullptr; hook->Notify = nullptr; }
                        { std::lock_guard<std::mutex> lock(registry.Gate); auto i = registry.Hooks.find(cookie); if (i != registry.Hooks.end() && i->second == hook) registry.Hooks.erase(i); }
                        return 0;
                    }
                    if (hook) return 3;
                    auto* source = {{sourceExpression}};
                    auto lifetime = DelegateBindings::Track(source);
                    std::unique_lock<std::recursive_mutex> sourceLock(lifetime->Gate, std::try_to_lock);
                    if (!sourceLock.owns_lock()) return 4;
                    if (!lifetime->Source) return 5;
                    hook = std::make_shared<{{bridge}}::Hook>(); hook->ContextId = context; hook->Cookie = cookie; hook->Target = target; hook->Notify = notify; hook->Lifetime = lifetime;
                    if (lifetime->Next == ~std::uint64_t(0)) return 2;
                    hook->Link = ++lifetime->Next;
                    lifetime->Closing.emplace(hook->Link, [hook](void* value) { hook->Closing(value); });
                    try {
                        { std::lock_guard<std::mutex> lock(lifetime->CopyGate); lifetime->NonCopyable.insert(hook.get()); }
                        { std::lock_guard<std::mutex> lock(registry.Gate); registry.Hooks.emplace(cookie, hook); }
                        source->template Bind<{{bridge}}::Hook, &{{bridge}}::Hook::Invoke>(hook.get());
                    } catch (...) {
                        try { source->template Unbind<{{bridge}}::Hook, &{{bridge}}::Hook::Invoke>(hook.get()); } catch (...) { }
                        lifetime->Closing.erase(hook->Link);
                        { std::lock_guard<std::mutex> lock(lifetime->CopyGate); lifetime->NonCopyable.erase(hook.get()); }
                        { std::lock_guard<std::mutex> lock(registry.Gate); registry.Hooks.erase(cookie); }
                        throw;
                    }
                    return 0;
                } catch (...) { return 2; }
            }
            extern "C" DLLExport int CallDeclaraton {{export}}_Drain(unsigned long long context) noexcept {
                if ({{bridge}}::CallbackDepth() != 0) return 4;
                try {
                    auto& registry = {{bridge}}::Get(); std::unique_lock<std::mutex> operation(registry.Operation, std::try_to_lock);
                    if (!operation.owns_lock()) return 4;
            #if defined(VET_EVENT_TEST_PROBES)
                    {{bridge}}::Probe probe(registry);
            #endif
                    { std::lock_guard<std::mutex> lock(registry.Gate); registry.Contexts.erase(context); }
                    for (;;) {
                        std::shared_ptr<{{bridge}}::Hook> hook;
                        { std::lock_guard<std::mutex> lock(registry.Gate); for (auto& value : registry.Hooks) if (value.second->ContextId == context) { hook = value.second; break; } }
                        if (!hook) return 0;
                        std::lock_guard<std::recursive_mutex> sourceLock(hook->Lifetime->Gate);
                        if (hook->Lifetime->Source) static_cast<{{bridge}}::Source*>(hook->Lifetime->Source)->template Unbind<{{bridge}}::Hook, &{{bridge}}::Hook::Invoke>(hook.get());
                        { std::lock_guard<std::mutex> lock(hook->Lifetime->CopyGate); hook->Lifetime->NonCopyable.erase(hook.get()); }
                        hook->Lifetime->Closing.erase(hook->Link);
                        { std::lock_guard<std::recursive_mutex> lock(hook->Gate); hook->Target = nullptr; hook->Notify = nullptr; }
                        { std::lock_guard<std::mutex> lock(registry.Gate); auto i = registry.Hooks.find(hook->Cookie); if (i != registry.Hooks.end() && i->second == hook) registry.Hooks.erase(i); }
                    }
                } catch (...) { return 2; }
            }
            #if defined(VET_EVENT_TEST_PROBES)
            extern "C" DLLExport int CallDeclaraton {{export}}_Busy() { return {{bridge}}::Get().Busy.load() ? 1 : 0; }
            #endif
            """);
    }

    internal static string EmitManaged(EmittedBinding entry, string export, string library, string suffix, EventInterop.Description description, string stem, string parameters, string values, string validation, string objects, Func<ConverterBodies, string> supportPath)
    {
        var field = (FieldDescriptor)entry.Native;
        bool instance = IsInstance(field);
        string name = field.Name.text, runtime = "global::Interop.__EventRuntime_" + suffix, action = description.ActionType;
        string wire = parameters.Length == 0 ? "" : ", " + parameters;
        string access = field.Api.ManagedAccess ?? "public";
        string modifier = instance ? "" : "static ", receiver = instance ? "this" : "null", ownerType = instance ? "global::FlaxEngine.Object" : "object";
        string addressSetup = instance ? "var address = global::FlaxEngine.Object.GetUnmanagedPtr(owner); if (address == 0) throw new global::System.ObjectDisposedException(\"Native event owner\");" : "nint address = 0;";
        string callbackReturn = description.Synchronous ? "int" : "void";
        string receive = description.Synchronous
            ? EventInterop.SynchronousReceive(description, stem, parameters, values, validation, objects, supportPath)
            : "private static void " + stem + "Receive(ulong cookie" + wire + ") { if (!" + stem + "Roots.TryGetValue(cookie, out var state) || !state.Slot.AcceptsEvents) return; try { " + validation + "\n" + objects + "\nstate.Slot.Emit(handler => handler(" + values + ")); } catch (global::System.Exception error) { state.Slot.Report(error); } }";
        string renewal = instance ? "state.Slot.NativeCancelled && global::FlaxEngine.Object.GetUnmanagedPtr(this) != 0" : "state.Slot.NativeCancelled";
        return $$"""
            {{entry.ManagedDeclaration}} { add => {{stem}}Get().Slot.Add(value); remove { var state = global::System.Threading.Volatile.Read(ref {{stem}}); if (state != null) state.Slot.Remove(value); } }
            {{access}} {{modifier}}int Pump{{name}}Events(int limit = 1024) => {{stem}}Get().Slot.Pump(limit);
            {{access}} {{modifier}}global::System.Exception {{name}}DispatchError => global::System.Threading.Volatile.Read(ref {{stem}})?.Slot.Error;
            {{access}} {{modifier}}void Shutdown{{name}}Events() => {{stem}}Get().Slot.Shutdown();
            private {{modifier}}{{stem}}State {{stem}};
            private {{modifier}}{{stem}}State {{stem}}Get() {
                var state = global::System.Threading.Volatile.Read(ref {{stem}});
                if (state == null || {{renewal}}) {
                    var created = new {{stem}}State({{receiver}});
                    var previous = global::System.Threading.Interlocked.CompareExchange(ref {{stem}}, created, state);
                    state = previous == state ? created : previous;
                }
                return state;
            }
            [global::System.Runtime.InteropServices.UnmanagedFunctionPointer(global::System.Runtime.InteropServices.CallingConvention.Cdecl)]
            private unsafe delegate {{callbackReturn}} {{stem}}Callback(ulong cookie{{wire}});
            [global::System.Runtime.InteropServices.UnmanagedFunctionPointer(global::System.Runtime.InteropServices.CallingConvention.Cdecl)]
            private delegate void {{stem}}ClosedCallback(ulong cookie);
            private static readonly {{stem}}Callback {{stem}}Root = {{stem}}CreateCallback();
            private static unsafe {{stem}}Callback {{stem}}CreateCallback() => {{stem}}Receive;
            private static readonly {{stem}}ClosedCallback {{stem}}ClosedRoot = {{stem}}Closed;
            private static readonly object {{stem}}ContextGate = new();
            private static ulong {{stem}}Context;
            private static bool {{stem}}Accepting = true;
            private static readonly global::System.Collections.Concurrent.ConcurrentDictionary<ulong, {{stem}}State> {{stem}}Roots = {{stem}}CreateRoots();
            private static ulong {{stem}}GetContext() {
                lock ({{stem}}ContextGate) {
                    if (!{{stem}}Accepting) throw new global::System.ObjectDisposedException("Native event module");
                    if ({{stem}}Context == 0) {
                        int status = {{stem}}NativeContext(global::System.Runtime.InteropServices.Marshal.GetFunctionPointerForDelegate({{stem}}Root), global::System.Runtime.InteropServices.Marshal.GetFunctionPointerForDelegate({{stem}}ClosedRoot), out {{stem}}Context);
                        if (status != 0) throw new global::System.InvalidOperationException("Native event context failed with status " + status);
                    }
                    return {{stem}}Context;
                }
            }
            private static global::System.Collections.Concurrent.ConcurrentDictionary<ulong, {{stem}}State> {{stem}}CreateRoots() {
                var roots = new global::System.Collections.Concurrent.ConcurrentDictionary<ulong, {{stem}}State>();
                var load = global::System.Runtime.Loader.AssemblyLoadContext.GetLoadContext(typeof({{stem}}State).Assembly);
                if (load != null) load.Unloading += _ => {
                    ulong context; lock ({{stem}}ContextGate) { {{stem}}Accepting = false; context = {{stem}}Context; }
                    foreach (var state in roots.Values) { try { state.Slot.Dispose(); } catch (global::System.Exception error) { state.Slot.Report(error); } }
                    if (context != 0 && {{stem}}NativeDrain(context) != 0) {
                        // Retain reverse-call code if native teardown fails;
                        // unloading its delegate thunks would be unsafe.
                        global::System.Runtime.InteropServices.GCHandle.Alloc({{stem}}Root);
                        global::System.Runtime.InteropServices.GCHandle.Alloc({{stem}}ClosedRoot);
                        global::System.Console.Error.WriteLine("Native event unload drain failed; callback roots retained.");
                        return;
                    }
                    roots.Clear();
                };
                return roots;
            }
            private sealed class {{stem}}State {
                private readonly {{ownerType}} owner;
                private ulong context, cookie;
                internal readonly {{runtime}}.Slot<{{action}}> Slot;
                internal {{stem}}State({{ownerType}} owner) { this.owner = owner; Slot = new(Bind, Unbind, false); }
                private void Bind() {
                    {{addressSetup}}
                    var nextContext = {{stem}}GetContext();
                    if (cookie != 0 && {{stem}}Roots.TryGetValue(cookie, out var previous) && global::System.Object.ReferenceEquals(previous, this)) {
                        Call(0, context, cookie, false); {{stem}}Roots.TryRemove(cookie, out _);
                    }
                    context = nextContext;
                    int status = {{stem}}NativeTicket(context, out cookie);
                    if (status != 0) throw new global::System.InvalidOperationException("Native event ticket failed with status " + status);
                    if (!{{stem}}Roots.TryAdd(cookie, this)) throw new global::System.InvalidOperationException("Duplicate event cookie");
                    try {
                        lock ({{stem}}ContextGate) { if (!{{stem}}Accepting) throw new global::System.ObjectDisposedException("Native event module"); }
                        Call(address, context, cookie, true);
                    } catch { {{stem}}Roots.TryRemove(cookie, out _); throw; }
                    global::System.GC.KeepAlive(owner);
                }
                private void Unbind() { Call(0, context, cookie, false); {{stem}}Roots.TryRemove(cookie, out _); }
                private static void Call(nint address, ulong context, ulong cookie, bool enable) {
                    int status = {{stem}}NativeBind(address, context, cookie, global::System.Runtime.InteropServices.Marshal.GetFunctionPointerForDelegate({{stem}}Root), global::System.Runtime.InteropServices.Marshal.GetFunctionPointerForDelegate({{stem}}ClosedRoot), enable ? 1 : 0);
                    if (status == 4) throw new global::System.InvalidOperationException("Native event subscription is busy (status 4); retry after the active callback returns.");
                    if (status != 0) throw new global::System.InvalidOperationException("Native instance event binding failed with status " + status);
                }
            }
            {{receive}}
            private static void {{stem}}Closed(ulong cookie) {
                if (!{{stem}}Roots.TryRemove(cookie, out var state)) return;
                try { state.Slot.CancelFromNative(); } catch (global::System.Exception error) { state.Slot.Report(error); }
            }
            [global::System.Runtime.InteropServices.DllImport({{System.Text.Json.JsonSerializer.Serialize(library)}}, EntryPoint = {{System.Text.Json.JsonSerializer.Serialize(export)}}, ExactSpelling = true, CallingConvention = global::System.Runtime.InteropServices.CallingConvention.Cdecl)]
            private static extern int {{stem}}NativeBind(nint owner, ulong context, ulong cookie, nint callback, nint closed, int enable);
            [global::System.Runtime.InteropServices.DllImport({{System.Text.Json.JsonSerializer.Serialize(library)}}, EntryPoint = {{System.Text.Json.JsonSerializer.Serialize(export + "_Context")}}, ExactSpelling = true, CallingConvention = global::System.Runtime.InteropServices.CallingConvention.Cdecl)]
            private static extern int {{stem}}NativeContext(nint callback, nint closed, out ulong context);
            [global::System.Runtime.InteropServices.DllImport({{System.Text.Json.JsonSerializer.Serialize(library)}}, EntryPoint = {{System.Text.Json.JsonSerializer.Serialize(export + "_Ticket")}}, ExactSpelling = true, CallingConvention = global::System.Runtime.InteropServices.CallingConvention.Cdecl)]
            private static extern int {{stem}}NativeTicket(ulong context, out ulong cookie);
            [global::System.Runtime.InteropServices.DllImport({{System.Text.Json.JsonSerializer.Serialize(library)}}, EntryPoint = {{System.Text.Json.JsonSerializer.Serialize(export + "_Drain")}}, ExactSpelling = true, CallingConvention = global::System.Runtime.InteropServices.CallingConvention.Cdecl)]
            private static extern int {{stem}}NativeDrain(ulong context);
            """;
    }
}
