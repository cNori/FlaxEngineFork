namespace VETools.Text;

// Flax's Windows GUID words use the System.Guid little-endian byte layout.
// Transfer words explicitly rather than relying on either struct's field layout.
internal static class FlaxGuidConverter
{
    public static void Register(TypeConverterRegistry registry) => registry.RegisterBodyConverter("Guid", "unsigned long long", new()
    {
        ManagedType = "global::System.Guid", ManagedAbiType = "ulong", CopyReferenceReturns = true, CopiedCallbackValue = true, CopyReferenceParameters = true,
        NativeRequirements = VETools.Generation.HandleNETcpp.CoTaskMemorySupport,
        ReleaseManagedAbi = c => $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem((nint)(nuint)({c.Value}));",
        ReleaseNativeAbi = c => $"::CoTaskMemFree(reinterpret_cast<void*>({c.Value}));",
        ManagedSupport = ManagedHelpers, NativeSupport = NativeHelpers,
        ManagedToAbi = c =>
        {
            string buffer = c.Unique("__guidBuffer");
            return new($"(ulong)(nuint){buffer}", $"nint {buffer} = 0;",
                $"{buffer} = global::{c.Support}.Write({c.Value});",
                $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem({buffer});");
        },
        AbiToManaged = c => new($"global::{c.Support}.Read((nint)(nuint)({c.Value}))",
            Cleanup: $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem((nint)(nuint)({c.Value}));"),
        AbiToNative = c => new($"::{c.Support}::Read({c.Value})"),
        NativeToAbi = c =>
        {
            string buffer = c.Unique("guidBuffer");
            return new(buffer, $"unsigned long long {buffer} = 0;",
                $"{buffer} = ::{c.Support}::Write({c.Value});",
                $"if (!{c.Completed}) ::CoTaskMemFree(reinterpret_cast<void*>({buffer}));");
        }
    }, nativeCppType: "::Guid", nativeDefault: "::Guid{}", existingManagedType: true);

    const string ManagedHelpers = """
        internal static class {Support}
        {
            internal static nint Write(global::System.Guid value)
            {
                byte[] bytes = value.ToByteArray();
                nint buffer = global::System.Runtime.InteropServices.Marshal.AllocCoTaskMem(16);
                try { global::System.Runtime.InteropServices.Marshal.Copy(bytes, 0, buffer, 16); return buffer; }
                catch { global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem(buffer); throw; }
            }
            internal static global::System.Guid Read(nint buffer)
            {
                if (buffer == 0) throw new global::System.InvalidOperationException("Null Guid buffer.");
                byte[] bytes = new byte[16];
                global::System.Runtime.InteropServices.Marshal.Copy(buffer, bytes, 0, 16);
                return new global::System.Guid(bytes);
            }
        }
        """;

    const string NativeHelpers = """
        namespace {Support} {
        static_assert(sizeof(unsigned int) == 4, "Guid converter requires 32-bit words.");
        inline ::Guid Read(unsigned long long address) {
            if (!address) throw std::invalid_argument("Null Guid buffer.");
            unsigned int words[4]; std::memcpy(words, reinterpret_cast<const void*>(address), 16);
            ::Guid value{}; value.A = words[0]; value.B = words[1]; value.C = words[2]; value.D = words[3]; return value;
        }
        inline unsigned long long Write(const ::Guid& value) {
            const unsigned int words[4] = { value.A, value.B, value.C, value.D };
            void* buffer = ::CoTaskMemAlloc(16); if (!buffer) throw std::bad_alloc();
            std::memcpy(buffer, words, 16); return reinterpret_cast<unsigned long long>(buffer);
        }
        }
        """;
}
