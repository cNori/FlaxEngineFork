# V2 bindings build integration

`Source/Tools/Flax.Bindings` owns the V2 parser, converters and call emitters. It is
a class library referenced directly by Flax.Build. The ToolingV2 console and its
regression runner reference that same library; their old source copies are no
longer compiled. Make generator changes in the engine-owned library.

Include indexing follows quoted project headers only. Literal `<...>` includes
and macro expansions to `<...>` are ignored, even when an include search root
contains the file. Known macros expanding to one quoted `.h` name remain supported;
unresolved include aliases such as FT_FREETYPE_H are skipped without chasing their
external configuration headers. No SDK/limits header constants are injected.
Explicit root/reference headers are still read when requested directly.

The default `-bindingsGenerator=V2` uses the resolved module headers, include
directories, definitions, C++ standard and binary-module name. Generated per-header
files live in `<module intermediate>/BindingsV2/`. Each C++ file is compiled as a
separate translation unit; each C# file enters the module's managed source list.
The old aggregate managed file is replaced with an inert comment. Native DLL call
arguments retain their original order even when public C# parameters are reordered.

V2 generation runs independently of the legacy parse-cache hit. It currently
regenerates in memory during build setup and publishes only changed output. A
failed V2 generation publishes no new V2 files and throws before scheduling its compile
tasks. Stale outputs are not substituted for failed generation. Unsupported ABI
targets fail explicitly; this initial backend supports Windows x64 and explicit
C++14/17/20 settings.

Native runtime registration, `TypeInitializer` definitions, serialization and
binary-module metadata still use the legacy generator. This also retains its
internal native wrappers for now. The new managed methods call V2 exports. This is
a transition adapter, not a complete replacement of Flax's reflection backend.

Build:

```powershell
dotnet build Source/Tools/Flax.Build/Flax.Build.csproj -c Release
.\Binaries\Tools\Flax.Build.exe -build -BuildBindingsOnly -buildtargets=FlaxEditor -platform=Windows -arch=x64 -configuration=Debug -bindingsGenerator=V2 -log
```

Omit `-BuildBindingsOnly` to compile native code too. Existing backend behavior
remains explicitly selectable with `-bindingsGenerator=Legacy`; V2 never silently
falls back on a generation error. Modules built without C# currently retain the
existing native-registration path.

Per-module backend stamps invalidate legacy parse-cache shortcuts when switching
between V2 and Legacy, so a V2 aggregate placeholder cannot masquerade as cached
legacy managed output.

Adapter checks:

```powershell
dotnet run --project Source/Tools/Flax.Bindings.Tests/Flax.Bindings.Tests.csproj
```

The engine-wide V2 build is not yet clean. The current probe advances through
Profiler and reaches remaining Platform binding diagnostics.
`API_FIELD(Private)` is now supported as a managed visibility override, including
`NoArray` storage; it does not change C++ access. Char arrays now have explicit
UTF-16 inline storage (two bytes per element).
Known static scripting declaration macros no longer prevent instance layout
calculation; ProfilerGPU::Event is verified against native size and field offsets.
Primitive fixed buffers now participate in automatic value conversion;
ProfilerMemory::GetGroups returns its inline GroupsArray without an ABI exclusion.
Fix the remaining generator/contract diagnostics before treating this as a working
editor build. `API_AUTO_SERIALIZATION` is preserved and skipped with warning
VETP004 until V2 reflection-based serialization is implemented. It no longer
blocks generation. Native runtime serialization still uses the retained legacy
registration path. `API_INJECT_CODE(cpp, "...")` now emits native code before
wrapper definitions, including adjacent string literals. Other injection languages
remain explicit errors. Private primitive/UTF-16 buffers now compose with record
and collection converters without exposing their managed fields. Explicitly exported
pointer fields carry borrowed addresses only; conversion never dereferences them or
transfers ownership. Profiler and CreateWindowSettings diagnostics from these cases
are resolved; cross-file base-class lifetimes and other ABI exclusions remain.

API metadata is separated into binding controls, managed declaration options,
managed attributes and extension tags. `API_FIELD(ReadOnly)` suppresses setters
for native-backed class fields; plain struct fields remain ordinary C# fields,
matching the legacy generator. Native `const` and `constexpr` are independent.
`Attributes="ReadOnly"` emits the editor attribute and does not change native
access or suppress a binding setter. `Tag="name=value"` is extension metadata,
not a C# attribute; repeated keys use the last value and known consumers such
as `NativeInvokeUseName` read the preserved tags.

Native field access is independent of shared memory layout. Exported structs with
unknown layouts use borrowed native views and typed C++ field accessors instead
of guessed offsets. Whole-value conversion remains a separate contract. Structs
with complete, public, non-virtual stateless interface bases can use field-by-field
value snapshots; their base/vtable bytes are never copied. Inherited data, unknown
interface state remain unsupported for automatic value conversion. `ISerializable` settings can therefore
cross as values or In/Out copies while serialization generation remains pending.
Scene object declaration macros also preserve their actual public access effect
for subsequent nested settings declarations.

Enum fields retain their managed enum types and use the proven native underlying
storage; they do not force an otherwise compatible record into a separate snapshot.
This permits enum fields alongside private fixed buffers in shared Ref records.
Top-level `Span<const T>` conversion copies into call-scoped, mutable backing
storage while preserving the const native Span signature. Results are copied
before that backing storage is released. Volatile elements, const Array storage
and nested Span lifetimes still require separate handlers.

Flax.Build opts into a project-wide class lifetime contract. Non-scripting base
wrappers receive a protected `(nint, SafeHandle)` constructor even when they have
no exported methods. Derived wrappers use a native upcast and share that owner
across generated files/modules. Single-header generation remains conservative
unless its caller explicitly enables this V2 dependency contract. Per-header
runtime helper names include the input identity to avoid empty-base collisions.

Static events may use converters explicitly marked `InlineCallbackValue` when
their wire scalar contains the complete copied value. These conversions cannot
require helper code, temporary storage or cleanup. Color32 uses this path;
native pointers, mutable references and instance-owner lifecycle requirements
are not relaxed.

Scripting declaration macros grant V2 access through the per-type
`VETBindingAccess<T>` friend, separately from legacy `TypeInternal` wrappers.
Generated specializations expose only pointers to explicitly exported members;
overload selection retains the complete native signature. This permits private
methods and fields without changing C++ visibility or adopting their storage.
Types without a supported friendship declaration remain rejected. The bridge
does not grant private constructor access or change object lifetime rules.

`BoundingFrustum` uses an owned packet of sixteen matrix floats. Native
`GetMatrix()` and the managed `Matrix` property supply the values, and each
receiver reconstructs its own frustum/derived planes. No private fields, union
storage, packing or native object bytes are copied. Writable shared references
remain unsupported; defensive In copies, value returns and Out copies use the
explicit converter.

Fixed `NoArray` record arrays use numbered managed snapshot fields and typed
native indexed accessors. Extent macros expand at the declaration's preprocessor
position; original tokens remain available for reconstruction. Conversion is
field-by-field, with bounded element/packet counts, name-collision rejection and
native `std::extent` assertions. Record arrays do not establish shared native
layout, and multidimensional/dynamic/qualified array cases still require their own handler.

`API_INJECT_CODE` accepts `cpp` and `csharp`. Managed file-scope snippets precede
generated namespaces; type-scope snippets stay inside the exported managed type
and survive interop rewriting. Adjacent quoted literals remain supported. Code
is preserved as code, including semicolons inside strings, rather than split
into individual statements.

`DECLARE_ASSET_HEADER` is a public declaration macro with the same per-type V2
friendship as the scripting macros. `SoftAssetReference<T>` projects to the
managed asset object, matching Flax's existing managed API. Native reads use
the soft reference's resolution path and may load the asset; inputs construct
a native reference without eagerly resolving it. Object identity, pointer
adjustment and engine-owned lifetime handling use the existing scripting bridge.
`SoftObjectReference<T>` retains its separate ID-preserving converter.

Static events also accept single pointers to scripting objects with one public,
non-virtual path to `ScriptingObject`. Native callbacks adjust the pointer to
the scripting root, and managed callbacks resolve the engine object identity
before creating a deferred work item. The queue retains that managed object,
never the borrowed native address. This does not extend native object lifetime:
Flax may invalidate the wrapper before delivery. Conversion failures are
reported through the event dispatch error without crossing the callback ABI.
Raw pointers, pointer slots/references and inaccessible base paths remain
unsupported.

The Flax profile enables `FlaxDelegateLifetimes`. Static events and instance
events on scripting objects then use native lifetime leases retired at the start
of `Delegate` destruction, before the callback list is released. Initial
subscription follows Flax's live-object calling contract; subsequent detach and
cancellation use the lease without dereferencing a destroyed owner. The base
`ScriptingObject.Deleted` event is not used for this purpose.

External subscriptions are excluded from native delegate copies and exported
binding lists. Moves, replacement and `UnbindAll` retire the old subscriptions;
live owners may subscribe to the replacement again. Native delegate layout does
not gain per-instance fields, and the untracked path avoids registry allocation.
The native hook is enabled for the current Win64 exception-enabled host profile;
other profiles retain no-op hooks. Emitting lifetime-aware V2 glue against a
disabled host hook is a compile-time error.

Per-context native tickets prevent cookie reuse across collectible managed
contexts. Context unload drains only that context's hooks; other contexts remain
subscribed. Failed teardown remains retryable. If unload cannot drain native
hooks, reverse-call delegates are retained and an error is logged. Native modules
must still be quiesced before their DLLs are unloaded. Standalone tests cover
owner deletion, native callback contention, source address reuse, copy/move
filtering, root release, and independent static/instance context unloading.

Explicit Ref on const T& emits C# ref readonly; unmarked const references retain
in. Explicit readonly references use pinned storage directly only when the ABI
has an established shared representation. Other converters retain their copies.

Automatic non-blittable struct Ref parameters use call-scoped copy-in/copy-back.
The native temporary receives the input fields, and successful calls replace the
managed value with converted output fields. Failed calls leave that managed value
unchanged. Input and output packet ownership is separate, including partial-output
and managed-conversion failures. These parameters do not preserve alias identity
between multiple arguments or permit native retention of the temporary. Reference
returns that could alias these temporaries are excluded. Compatible shared-layout
structs retain direct Ref storage; custom codecs must explicitly opt into
CopyReferenceParameters. Native class snapshots do not opt in automatically.
Const-reference event payloads can use an explicit CopiedCallbackValue converter.
Automatic exported struct snapshots opt in when all fields are approved scalar,
enum or engine-object identities; raw pointer fields do not qualify. Native glue
creates a call-scoped packet and releases it after the reverse callback, including
failure paths. Managed glue decodes an independent snapshot before queuing it and
does not free the borrowed packet. Nested records and fixed contact arrays are
copied field by field; engine object pointers become managed identities immediately.
Writable/rvalue event references still require a synchronous ref/out adapter;
deferred delivery cannot modify the already-returned native argument.
In the Flax host profile, single MArray*/const MArray* parameters map to
System.Array through SystemArrayMarshaller.ManagedToNative. Its pooled wrapper
is released in finally. MArray* results use NativeToManaged and release the
returned owned host wrapper after decoding, including managed decode failures.
These are opaque runtime handles; the glue never treats a movable managed array
as a native object address. MType* uses SystemTypeMarshaller and its cached type
identity; cached type handles are not freed. Native parameter types and overload
selection remain unchanged. References, pointer slots, deep pointers and volatile
runtime-handle shapes require an explicit ownership adapter. Outside the host
profile, these names do not activate managed-object conversion.
Native-owned inline scripting-object collections expose host identities rather
than copying their elements. Array<T> requires a readonly native reference return;
Span<T> may return its borrowed view by value. Inputs, writable collection refs,
owning Array<T> value returns and nested snapshot composition remain excluded.
The native owner controls element lifetime and collection invalidation.

Fixed primitive buffers in independent snapshots are flattened into numbered
fields, with explicit per-element native paths and compiler extent assertions.
Internal fields retain internal managed access. Direct shared-layout buffers
continue to use their existing inline representation; private snapshot buffers
are not exposed through this conversion.

API_EVENT(Internal) preserves internal access for the event and dispatch helpers.
Non-public native events require a recognized scripting friendship declaration
and the Delegate lifetime bridge. Generated VETBindingAccess specializations
provide access without changing native visibility. Guid callbacks use the
explicit word-based converter and produce independent System.Guid values before
queuing; native callback packets remain native-owned.

Explicitly registered synchronous events run only on the subscription thread.
Writable value refs use copy-in/copy-back before native execution resumes.
Wrong-thread calls are rejected and reported. Subscription changes from inside a
callback can return busy; teardown must be retried after the callback returns.
Queued events retain their separate asynchronous owner-thread behavior.

Variant rvalue inputs construct an independent native value from the managed
packet and move that temporary into the call. This does not expose or move a
borrowed native Variant; other rvalue types still need an explicit MoveInputs
converter contract.

API_STRUCT(NoPod) explicitly disables inferred shared native layouts; supported fields use independent value conversion. Metadata keyword arguments do not act as native storage specifiers. String and StringView event payloads are decoded to independent managed strings before queued delivery.


The Flax.Build adapter explicitly retains legacy native RPC registration for
Hidden NetworkRpc methods. V2 preserves the target metadata and emits no local
call wrapper for these declarations. Standalone generation, public RPC wrappers
and missing or contradictory Server/Client targets remain unsupported errors.

Function callback parameters may carry a single unqualified void* as nint.
These are borrowed opaque addresses, never managed objects or retained pointee
storage. Typed pointers, deep pointers and pointer returns still require a
separate callback lifetime converter.

Half4 uses four explicit sixteen-bit words packed into an unsigned scalar wire
value. Managed Half.RawValue and bit-preserving reconstruction avoid converting
NaNs, signed zero or payload bits through float. Guid writable references use
the established copy-in/copy-back path. Constructors with defaults on every
parameter are recognized as callable without arguments.

API_PROPERTY setters with a supported T& parameter call the native method with
the managed setter-local value by reference. Native changes affect that local,
not the original caller variable; direct method calls retain their usual ref
copyback behavior. Pointer-slot and Out setters are not covered by this rule.

NetworkManager.ClientConnecting uses an explicit synchronous validation policy.
On managed handler, thread-affinity or conversion failure, native Result is set
to rejection before networking continues. This failure action is a host-registered
contract and is part of the generated callback identity. Other synchronous events
keep their separately registered failure behavior.

Qualified native enum-member defaults are retained as fully qualified C# enum constants. The qualifier is resolved in the parameter's native scope and the member is checked against that enum; unrelated enum types and missing members remain unsupported-default warnings.


Single unqualified pointers to API_INTERFACE types with a scripting-type marker
use Flax's scripting-object identity bridge. Native ToInterface/FromInterface
performs pointer adjustment; managed values remain interface references backed
by FlaxEngine.Object. Invalid object/interface conversions are rejected rather
than treated as raw addresses. This does not transfer ownership. Interface value
copies, const/deep pointers and pointer slots still require separate contracts.

RegisterExportedSettingsSnapshot explicitly selects a settings DTO made from
exported fields. ModelTool::Options uses this contract: ImportTypes and Cached
stay native. Direct native field setters assign the selected fields individually
instead of replacing the whole native object, preserving transient cache state.
Reconstructed input temporaries receive default internal state; general writable
ref/callback copyback is disabled for these selected-field snapshots and records
that contain them. Other automatic records still require complete field support.

API_ENUM(Internal) emits an internal managed enum at both top-level and nested locations. Native enum visibility is unchanged.

Engine.ReportCrash has an explicit synchronous borrowed-pointer contract: the error message becomes a managed string, while the context is an opaque nint valid only during that callback. The context is never queued, retained or freed by the bridge. Delivery is restricted to the subscription thread. Rejected, failed or throwing callbacks use Platform.Error as native fallback. Other raw event pointers remain unsupported unless their synchronous lifetime contract is registered explicitly.


Module generation separates converter value declarations from per-header glue. Identical value declarations use a content-addressed source file in the assembly's shared output directory; converter helper partials stay with each header. The compiler and generated project source lists deduplicate shared paths. Standalone header previews remain self-contained. Root headers use Values as a fallback namespace so snapshots cannot collide with their native access views.

