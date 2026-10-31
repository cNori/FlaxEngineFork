namespace VETools.Text;

// Explicit UTF-8 byte-view convention, including embedded NULs and byte lengths.
internal static class FlaxAnsiViewConverter
{
    public static void Register(TypeConverterRegistry registry)
    {
        Register(registry, "StringAnsiView");
        Register(registry, "StringAnsi");
    }
    static void Register(TypeConverterRegistry registry, string native) => registry.RegisterBodyConverter(native, "unsigned long long", new()
    {
        ManagedType = "string", ManagedAbiType = "ulong", CopyReferenceReturns = true,
        ManagedSupport = Managed, NativeSupport = Native,
        NativeRequirements = VETools.Generation.HandleNETcpp.CoTaskMemorySupport,
        ManagedToAbi = context =>
        {
            string buffer = context.Unique("__utf8");
            return new($"(ulong)(nuint){buffer}", $"nint {buffer} = 0;", $"{buffer} = global::{context.Support}.Allocate({context.Value});", $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem({buffer});");
        },
        AbiToManaged = context => new($"global::{context.Support}.Read((nint)(nuint)({context.Value}))", Cleanup: $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem((nint)(nuint)({context.Value}));"),
        AbiToNative = context => new($"::{native}(::{context.Support}::Data({context.Value}), ::{context.Support}::Length({context.Value}))"),
        NativeToAbi = context =>
        {
            string buffer = context.Unique("utf8Buffer");
            return new(buffer, $"unsigned long long {buffer} = 0;", $"{buffer} = ::{context.Support}::Copy(({context.Value}).Get(), ({context.Value}).Length());", $"if (!{context.Completed}) ::CoTaskMemFree(reinterpret_cast<void*>({buffer}));");
        },
        ReleaseManagedAbi = context => $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem((nint)(nuint)({context.Value}));",
        ReleaseNativeAbi = context => $"::CoTaskMemFree(reinterpret_cast<void*>({context.Value}));"
    }, nativeCppType: "::" + native, nativeDefault: "::" + native + "{}");

    const string Managed = """
        internal static class {Support} {
        internal static nint Allocate(string text) {
            byte[] bytes = global::System.Text.Encoding.UTF8.GetBytes(text ?? "");
            nint address = global::System.Runtime.InteropServices.Marshal.AllocCoTaskMem(checked(bytes.Length + 5));
            try { global::System.Runtime.InteropServices.Marshal.WriteInt32(address, bytes.Length); global::System.Runtime.InteropServices.Marshal.Copy(bytes, 0, address + 4, bytes.Length); global::System.Runtime.InteropServices.Marshal.WriteByte(address + 4 + bytes.Length, 0); return address; }
            catch { global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem(address); throw; }
        }
        internal static string Read(nint address) {
            if (address == 0) return "";
            int count = global::System.Runtime.InteropServices.Marshal.ReadInt32(address);
            if (count < 0 || count > int.MaxValue - 5) throw new global::System.InvalidOperationException("Invalid UTF-8 byte length.");
            return global::System.Runtime.InteropServices.Marshal.PtrToStringUTF8(address + 4, count);
        }
        }
        """;
    const string Native = """
        namespace {Support} {
        inline int Length(unsigned long long address) { if (!address) return 0; int count; std::memcpy(&count, reinterpret_cast<void*>(address), 4); if (count < 0 || count > 0x7fffffff - 5) throw std::invalid_argument("Invalid UTF-8 byte length."); return count; }
        inline const char* Data(unsigned long long address) { return address ? reinterpret_cast<const char*>(address) + 4 : ""; }
        inline unsigned long long Copy(const char* data, int count) { if (count < 0 || count > 0x7fffffff - 5 || (count && !data)) throw std::invalid_argument("Invalid UTF-8 view."); auto* result = static_cast<unsigned char*>(::CoTaskMemAlloc(count + 5)); if (!result) throw std::bad_alloc(); std::memcpy(result, &count, 4); if (count) std::memcpy(result + 4, data, count); result[count + 4] = 0; return reinterpret_cast<unsigned long long>(result); }
        }
        """;
}
