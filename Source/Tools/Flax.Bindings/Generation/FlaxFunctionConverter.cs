using System.Text;
using VETools.Text;
using TypeInfo = VETools.Text.TypeInfo;

namespace VETools.Generation;

// A function packet owns one lease. Both runtimes may retain a distinct lease;
// packet cleanup never invalidates a Function copy or a managed delegate proxy.
internal static class FlaxFunctionConverter
{
    sealed record Slot(string Native, string Managed, string Cpp, string CS, bool Boolean, bool Character, bool Enum, string Identity);

    public static NativeTypeConverter? Create(NamedTypeInfo named, DeclarationDescriptor context)
    {
        if(context.SourceFile?.Converters.FlaxFunctions != true || (named.Symbol?.QualifiedName ?? named.Name.TrimStart(':')) != "Function" || named.Arguments.Count != 1) return null;
        var tokens=named.Arguments[0].Tokens.Where(t=>!t.IsTrivia).ToList();
        int open=tokens.FindIndex(t=>t=="(");
        if(open<=0 || tokens[^1]!=")") return null;
        var analysis=context.SourceFile.TypeInfos!;
        TypeInfo Parse(List<VETools.Tokenizer.Token> ts)=>TypeInfoPass.Parse(ts,context,analysis);
        var returns=Parse(tokens.Take(open).ToList());
        bool isVoid=returns is NamedTypeInfo { Name:"void",Resolution:TypeResolution.Builtin };
        var result=isVoid?null:Describe(returns,false); if(!isVoid && result==null) return null;
        List<Slot> args=[]; List<VETools.Tokenizer.Token> part=[];
        var inner=tokens.Skip(open+1).SkipLast(1).ToArray();
        if(inner.Length==1 && inner[0]=="void") inner=[];
        foreach(var token in inner.Append(new VETools.Tokenizer.Token { text=",",kind=VETools.Tokenizer.Token.Kind.ControlChar }))
        {
            if(token==",") { if(part.Count==0) { if(inner.Length!=0)return null; } else {var slot=Describe(Parse(part));if(slot==null)return null;args.Add(slot);part=[];} }
            else part.Add(token);
        }
        if(args.Count>16) return null;
        string native="::Function<"+(isVoid?"void":result!.Native)+"("+string.Join(", ",args.Select(a=>a.Native))+")>";
        string managed=isVoid?"global::System.Action"+(args.Count==0?"":"<"+string.Join(", ",args.Select(a=>a.Managed))+">"):
            "global::System.Func<"+string.Join(", ",args.Select(a=>a.Managed).Append(result!.Managed))+">";
        return new()
        {
            NativeName=named.Display,NativeCppType=native,AbiCppType="unsigned long long",NativeDefault=native+"{}",ExistingManagedType=true,FunctionValue=true,
            NativeSignatureIdentity="function:"+(isVoid?"void":result!.Identity)+"("+string.Join(",",args.Select(a=>a.Identity))+")",
            ToAbi=_=>throw new InvalidOperationException(),ToNative=_=>throw new InvalidOperationException(),
            Bodies=new()
            {
                ManagedType=managed,ManagedAbiType="ulong",CopyReferenceReturns=true,
                NativeRequirements=NativeSupportRequirements.Merge([HandleNETcpp.CoTaskMemorySupport,new() { Includes=["<memory>","<atomic>","\"Engine/Core/Delegate.h\""] }]),
                ManagedSupport=Managed(args,result,managed),NativeSupport=Native(args,result,native),
                ReleaseManagedAbi=c=>$"global::{c.Support}.Free((nint)(nuint)({c.Value}));",
                ReleaseNativeAbi=c=>$"::{c.Support}::Free({c.Value});",
                ManagedToAbi=c=> {string packet=c.Unique("__functionPacket");return new($"(ulong)(nuint){packet}",$"nint {packet}=0;",$"{packet}=global::{c.Support}.Write({c.Value});",$"global::{c.Support}.Finish({packet}, {c.CallSucceeded});");},
                AbiToManaged=c=>new($"global::{c.Support}.Read((nint)(nuint)({c.Value}))",Cleanup:$"global::{c.Support}.Free((nint)(nuint)({c.Value}));"),
                AbiToNative=c=>new($"::{c.Support}::Read({c.Value})"),
                NativeToAbi=c=> {string packet=c.Unique("functionPacket");return new(packet,$"unsigned long long {packet}=0;",$"{packet}=::{c.Support}::Write({c.Value});",$"if(!{c.Completed}) ::{c.Support}::Free({packet});");}
            }
        };
    }

    static Slot? Describe(TypeInfo type,bool parameter=true)
    {
        // Opaque callback arguments carry an address only. The callback owns no
        // pointee storage and the bridge never dereferences or retains it.
        if (parameter && type is PointerTypeInfo { Qualifiers: TypeQualifiers.None, Pointee: NamedTypeInfo { Name: "void", Resolution: TypeResolution.Builtin, EffectiveQualifiers: TypeQualifiers.None } })
            return new("void*", "nint", "void*", "nint", false, false, false, "void*");
        if(type is not NamedTypeInfo named || (named.EffectiveQualifiers & TypeQualifiers.Volatile)!=0)return null;
        var scalar=ScalarAbi.Resolve(type); string? managed=scalar?.CS; bool enumeration=false;
        if(named.Symbol?.Definition is EnumDescriptor e && e.Api.Export)
        { scalar=e.UnderlyingType==null?e.IsScoped?new("int","int",4):null:ScalarAbi.Resolve(e.UnderlyingType);managed=HandleNetCS.ManagedName(e);enumeration=true; }
        if(named.Name.TrimStart(':')=="Char" && named.Converter?.Bodies?.ManagedType=="char") {scalar=new("unsigned short","char",2);managed="char";}
        if(scalar==null || managed==null)return null;
        return new(TemplateInstantiationPass.Native(type),managed,scalar.CS=="bool"?"unsigned char":scalar.CS=="char"?"unsigned short":scalar.Cpp,
            scalar.CS=="bool"?"byte":scalar.CS=="char"?"ushort":scalar.CS,scalar.CS=="bool",scalar.CS=="char",enumeration,BindingSignaturePass.Canonical(type,parameter)??TemplateInstantiationPass.Native(type));
    }
    static string ToCS(Slot s,string v)=>s.Boolean?$"({v} != 0)":s.Character||s.Enum?$"({s.Managed})({v})":v;
    static string FromCS(Slot s,string v)=>s.Boolean?$"(byte)({v} ? 1 : 0)":$"({s.CS})({v})";
    static string ToCPP(Slot s,string v)=>$"static_cast<{s.Native}>({v})";
    static string FromCPP(Slot s,string v)=>$"static_cast<{s.Cpp}>({v})";

    static string Managed(List<Slot> args,Slot? result,string managed)
    {
        string wireArgs=string.Join("",args.Select((a,i)=>$", {a.CS} arg{i}"));
        string wireOut=result==null?"":$", out {result.CS} result";
        string callbackArgs=string.Join(", ",args.Select((a,i)=>ToCS(a,"arg"+i)));
        string call=result==null?$"owner.Target({callbackArgs});":$"result={FromCS(result,$"owner.Target({callbackArgs})")};";
        string validate=string.Join("\n",args.Select((a,i)=>a.Boolean?$"if(arg{i}>1) throw new global::System.InvalidOperationException(\"Invalid callback boolean\");":""));
        string proxyArgs=string.Join(", ",args.Select((a,i)=>a.Managed+" arg"+i));
        string wireValues=string.Join("",args.Select((a,i)=>", "+FromCS(a,"arg"+i)));
        string outArg=result==null?"":", out var result";
        string returnValue=result==null?"":(result.Boolean?"if(result>1) throw new global::System.InvalidOperationException(\"Invalid callback boolean result\");\n":"")+"return "+ToCS(result,"result")+";";
        return $$"""
        internal static class {Support} {
            [global::System.Runtime.InteropServices.UnmanagedFunctionPointer(global::System.Runtime.InteropServices.CallingConvention.Cdecl)]
            private delegate int InvokeTarget(nint context{{wireArgs}}{{wireOut}});
            [global::System.Runtime.InteropServices.UnmanagedFunctionPointer(global::System.Runtime.InteropServices.CallingConvention.Cdecl)]
            private delegate void Lifetime(nint context);
            private static readonly InvokeTarget InvokeRoot=InvokeManaged;
            private static readonly Lifetime RetainRoot=RetainManaged, ReleaseRoot=ReleaseManaged;
            private sealed class Owner {
                internal readonly {{managed}} Target;
                internal global::System.Runtime.InteropServices.GCHandle Handle;
                internal int References=1;
                internal global::System.Runtime.ExceptionServices.ExceptionDispatchInfo Error;
                internal global::System.Runtime.ExceptionServices.ExceptionDispatchInfo InlineError;
                internal readonly global::System.Threading.Thread CallerThread=global::System.Threading.Thread.CurrentThread;
                internal Owner({{managed}} target) { Target=target; Handle=global::System.Runtime.InteropServices.GCHandle.Alloc(this); }
                internal void Release() { if(global::System.Threading.Interlocked.Decrement(ref References)==0) Handle.Free(); }
                internal void Fail(global::System.Exception error) {
                    var captured=global::System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(error);
                    if(global::System.Object.ReferenceEquals(CallerThread,global::System.Threading.Thread.CurrentThread))
                        global::System.Threading.Interlocked.CompareExchange(ref InlineError,captured,null);
                    if(global::System.Threading.Interlocked.CompareExchange(ref Error,captured,null)==null) {
                        try { global::System.Diagnostics.Trace.TraceError(error.ToString()); } catch { }
                        try { global::System.Console.Error.WriteLine(error.ToString()); } catch { }
                    }
                }
            }
            private static Owner Get(nint context)=>(Owner)global::System.Runtime.InteropServices.GCHandle.FromIntPtr(context).Target;
            private static void RetainManaged(nint context) { try { global::System.Threading.Interlocked.Increment(ref Get(context).References); } catch { } }
            private static void ReleaseManaged(nint context) { try { Get(context).Release(); } catch { } }
            private static int InvokeManaged(nint context{{wireArgs}}{{wireOut}}) {
                {{(result==null?"":"result=default;")}}
                Owner owner=null;
                try { owner=Get(context); {{validate}} {{call}} return 1; }
                catch(global::System.Exception error) { try { owner?.Fail(error); } catch { } return 0; }
            }
            internal static nint Write({{managed}} value) {
                if(value==null)return 0;
                if(value.GetInvocationList().Length!=1) throw new global::System.ArgumentException("Native Function requires one delegate invocation target.",nameof(value));
                var owner=new Owner(value); nint packet=0;
                try {
                    packet=global::System.Runtime.InteropServices.Marshal.AllocCoTaskMem(32);
                    global::System.Runtime.InteropServices.Marshal.WriteIntPtr(packet,0,global::System.Runtime.InteropServices.Marshal.GetFunctionPointerForDelegate(InvokeRoot));
                    global::System.Runtime.InteropServices.Marshal.WriteIntPtr(packet,8,global::System.Runtime.InteropServices.GCHandle.ToIntPtr(owner.Handle));
                    global::System.Runtime.InteropServices.Marshal.WriteIntPtr(packet,16,global::System.Runtime.InteropServices.Marshal.GetFunctionPointerForDelegate(RetainRoot));
                    global::System.Runtime.InteropServices.Marshal.WriteIntPtr(packet,24,global::System.Runtime.InteropServices.Marshal.GetFunctionPointerForDelegate(ReleaseRoot));
                    return packet;
                } catch { owner.Release(); global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem(packet); throw; }
            }
            internal static void Free(nint packet) {
                if(packet==0)return;
                try { var release=global::System.Runtime.InteropServices.Marshal.GetDelegateForFunctionPointer<Lifetime>(global::System.Runtime.InteropServices.Marshal.ReadIntPtr(packet,24)); release(global::System.Runtime.InteropServices.Marshal.ReadIntPtr(packet,8)); }
                finally { global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem(packet); }
            }
            internal static void Finish(nint packet,bool succeeded) {
                if(packet==0)return;
                try { if(succeeded) Get(global::System.Runtime.InteropServices.Marshal.ReadIntPtr(packet,8)).InlineError?.Throw(); }
                finally { Free(packet); }
            }
            private sealed class NativeLease : global::System.Runtime.InteropServices.SafeHandle {
                readonly Lifetime release;
                internal readonly InvokeTarget Invoke;
                internal NativeLease(nint packet):base(0,true) {
                    nint context=global::System.Runtime.InteropServices.Marshal.ReadIntPtr(packet,8);
                    if(context==0)throw new global::System.InvalidOperationException("Null Function context.");
                    Invoke=global::System.Runtime.InteropServices.Marshal.GetDelegateForFunctionPointer<InvokeTarget>(global::System.Runtime.InteropServices.Marshal.ReadIntPtr(packet,0));
                    release=global::System.Runtime.InteropServices.Marshal.GetDelegateForFunctionPointer<Lifetime>(global::System.Runtime.InteropServices.Marshal.ReadIntPtr(packet,24));
                    var retain=global::System.Runtime.InteropServices.Marshal.GetDelegateForFunctionPointer<Lifetime>(global::System.Runtime.InteropServices.Marshal.ReadIntPtr(packet,16));
                    retain(context); SetHandle(context);
                }
                public override bool IsInvalid=>handle==0;
                protected override bool ReleaseHandle() { try { release(handle); return true; } catch { return false; } }
            }
            internal static {{managed}} Read(nint packet) {
                if(packet==0)return null;
                var lease=new NativeLease(packet);
                return ({{proxyArgs}})=> {
                    try { if(lease.Invoke(lease.DangerousGetHandle(){{wireValues}}{{outArg}})==0) throw new global::System.InvalidOperationException("Native Function invocation failed."); {{returnValue}} }
                    finally { global::System.GC.KeepAlive(lease); }
                };
            }
        }
        """;
    }

    static string Native(List<Slot> args,Slot? result,string native)
    {
        string cppArgs=string.Join("",args.Select((a,i)=>", "+a.Cpp+" arg"+i));
        string outType=result==null?"":", "+result.Cpp+"* result";
        string nativeArgs=string.Join(", ",args.Select((a,i)=>a.Native+" arg"+i));
        string toWire=string.Join("",args.Select((a,i)=>", "+FromCPP(a,"arg"+i)));
        string fromWire=string.Join(", ",args.Select((a,i)=>ToCPP(a,"arg"+i)));
        string returnType=result?.Native??"void";
        string fallback=result==null?"return;":"return {};";
        string local=result==null?"":$"{result.Cpp} result{{}};";
        string ret=result==null?"":(result.Boolean?"if(result>1) { Failed()=true; return false; }":"")+"return "+ToCPP(result,"result")+";";
        string sourceCall=result==null?$"state->TargetValue({fromWire});":$"*result={FromCPP(result,$"state->TargetValue({fromWire})")};";
        string validate=string.Join("\n",args.Select((a,i)=>a.Boolean?$"if(arg{i}>1)return 0;":""));
        return $$"""
        namespace {Support} {
            using Target={{native}};
            using Invoke=int(CallDeclaraton*)(void*{{string.Join("",args.Select(a=>", "+a.Cpp))}}{{(result==null?"":", "+result.Cpp+"*")}});
            using Lifetime=void(CallDeclaraton*)(void*);
            struct Packet { Invoke Call; void* Context; Lifetime Retain; Lifetime Release; };
            static_assert(sizeof(Packet)==32 && sizeof(void*)==8,"Function packet requires Windows x64");
            inline bool& Failed() { static thread_local bool value=false; return value; }
            struct ForeignLease {
                Packet Value;
                explicit ForeignLease(Packet value):Value(value) { Value.Retain(Value.Context); }
                ~ForeignLease() { Value.Release(Value.Context); }
            };
            inline Target Read(unsigned long long address) {
                if(!address)return Target{};
                Packet packet; std::memcpy(&packet,reinterpret_cast<const void*>(address),32);
                if(!packet.Call || !packet.Context || !packet.Retain || !packet.Release)throw std::invalid_argument("Invalid Function packet");
                auto lease=std::make_shared<ForeignLease>(packet);
                return Target([lease]({{nativeArgs}})->{{returnType}} {
                    auto alive=lease; {{local}}
                    if(!alive->Value.Call(alive->Value.Context{{toWire}}{{(result==null?"":", &result")}})) { Failed()=true; {{fallback}} }
                    {{ret}}
                });
            }
            struct NativeState {
                std::atomic<int> References{1}; Target TargetValue;
                explicit NativeState(const Target& value):TargetValue(value) {}
            };
            inline void CallDeclaraton Retain(void* context) noexcept { static_cast<NativeState*>(context)->References.fetch_add(1,std::memory_order_relaxed); }
            inline void CallDeclaraton Release(void* context) noexcept { auto* state=static_cast<NativeState*>(context); if(state->References.fetch_sub(1,std::memory_order_acq_rel)==1)delete state; }
            inline int CallDeclaraton Call(void* context{{cppArgs}}{{outType}}) noexcept {
                if(!context{{(result==null?"":" || !result")}})return 0;
                {{validate}}
                auto* state=static_cast<NativeState*>(context);
                bool previous=Failed(); Failed()=false;
                try { {{sourceCall}} bool failed=Failed(); Failed()=previous||failed; return failed?0:1; }
                catch(...) { Failed()=previous; return 0; }
            }
            inline unsigned long long Write(const Target& value) {
                if(!value.IsBinded())return 0;
                auto* state=new NativeState(value);
                auto* packet=static_cast<Packet*>(::CoTaskMemAlloc(32));
                if(!packet) { delete state; throw std::bad_alloc(); }
                *packet={&Call,state,&Retain,&Release}; return reinterpret_cast<unsigned long long>(packet);
            }
            inline void Free(unsigned long long address) {
                if(!address)return; auto* packet=reinterpret_cast<Packet*>(address);
                packet->Release(packet->Context); ::CoTaskMemFree(packet);
            }
        }
        """;
    }
}
