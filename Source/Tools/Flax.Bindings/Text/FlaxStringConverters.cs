namespace VETools.Text;

internal static class FlaxStringConverters
{
    public static void RegisterWindowsX64(TypeConverterRegistry registry)
    {
        foreach (string type in new[] { "String", "StringView" })
            registry.RegisterBodyConverter(type, "unsigned long long", new()
            {
                ManagedType = "string", ManagedAbiType = "ulong", CopyReferenceReturns = true, CopiedCallbackValue = true, MoveInputs = type == "String",
                ManagedOptionalDefault = expression => expression == "null" ? "null" : expression.TrimStart(':') == type + "::Empty" ? "\"\"" : null,
                ReleaseManagedAbi = context => $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem((nint)(nuint)({context.Value}));",
                ReleaseNativeAbi = context => $"::CoTaskMemFree(reinterpret_cast<void*>({context.Value}));",
                ManagedSupport = ManagedHelpers, NativeSupport = NativeHelpers,
                NativeRequirements = VETools.Generation.HandleNETcpp.CoTaskMemorySupport,
                ManagedToAbi = context =>
                {
                    string buffer = context.Unique("__textBuffer");
                    return new($"(ulong)(nuint){buffer}", Declarations: $"nint {buffer} = 0;",
                        Setup: $"{buffer} = global::{context.Support}.Allocate({context.Value});",
                        Cleanup: $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem({buffer});");
                },
                AbiToManaged = context => new($"global::{context.Support}.Read((nint)(nuint)({context.Value}))",
                    Cleanup: $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem((nint)(nuint)({context.Value}));"),
                AbiToNative = context =>
                {
                    string length = context.Unique("length");
                    return new($"::{type}(::{context.Support}::Data({context.Value}), {length})",
                        Setup: $"int {length} = ::{context.Support}::Length({context.Value});");
                },
                NativeToAbi = context =>
                {
                    string buffer = context.Unique("textBuffer");
                    return new(buffer, Declarations: $"unsigned long long {buffer} = 0;",
                        Setup: $"{buffer} = ::{context.Support}::Copy(({context.Value}).Get(), ({context.Value}).Length());",
                        Cleanup: $"if (!{context.Completed}) ::{context.Support}::Free({buffer});");
                }
            }, nativeCppType: "::" + type, nativeDefault: "::" + type + "{}");
        FlaxAnsiViewConverter.Register(registry);
    }

    // Explicit UTF-16 length + characters + terminator. Null managed input becomes an empty native string.
    const string ManagedHelpers = """
        internal static class {Support}
        {
            internal static nint Allocate(string text)
            {
                text ??= global::System.String.Empty;
                int size = checked(6 + text.Length * 2);
                nint address = global::System.Runtime.InteropServices.Marshal.AllocCoTaskMem(size);
                try
                {
                    global::System.Runtime.InteropServices.Marshal.WriteInt32(address, text.Length);
                    global::System.Runtime.InteropServices.Marshal.Copy(text.ToCharArray(), 0, address + 4, text.Length);
                    global::System.Runtime.InteropServices.Marshal.WriteInt16(address + 4 + text.Length * 2, 0);
                    return address;
                }
                catch { global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem(address); throw; }
            }
            internal static string Read(nint address)
            {
                if (address == 0) return global::System.String.Empty;
                int length = global::System.Runtime.InteropServices.Marshal.ReadInt32(address);
                if (length < 0 || length > (global::System.Int32.MaxValue - 6) / 2) throw new global::System.InvalidOperationException("Invalid UTF-16 ABI length.");
                return global::System.Runtime.InteropServices.Marshal.PtrToStringUni(address + 4, length);
            }
        }
        """;

    const string NativeHelpers = """
        namespace {Support}
        {
            static_assert(sizeof(Char) == 2 && sizeof(int) == 4 && sizeof(void*) == 8, "UTF-16 Windows x64 ABI required.");
            inline int Length(unsigned long long address)
            {
                if (!address) return 0;
                int length; std::memcpy(&length, reinterpret_cast<const void*>(address), 4);
                if (length < 0 || length > (0x7fffffff - 6) / 2) throw std::invalid_argument("Invalid UTF-16 ABI length.");
                return length;
            }
            inline const Char* Data(unsigned long long address)
            {
                static const Char empty[1] = {};
                return address ? reinterpret_cast<const Char*>(reinterpret_cast<const unsigned char*>(address) + 4) : empty;
            }
            inline unsigned long long Copy(const Char* text, int length)
            {
                if (length < 0 || length > (0x7fffffff - 6) / 2 || (length && !text)) throw std::invalid_argument("Invalid native UTF-16 string.");
                auto size = static_cast<std::size_t>(length) * 2;
                auto* address = static_cast<unsigned char*>(::CoTaskMemAlloc(size + 6));
                if (!address) throw std::bad_alloc();
                std::memcpy(address, &length, 4);
                if (length) std::memcpy(address + 4, text, size);
                address[4 + size] = address[5 + size] = 0;
                return reinterpret_cast<unsigned long long>(address);
            }
            inline void Free(unsigned long long address) { ::CoTaskMemFree(reinterpret_cast<void*>(address)); }
        }
        """;
}
