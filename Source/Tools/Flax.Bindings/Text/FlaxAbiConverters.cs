namespace VETools.Text;

// Explicit representations for .NET-backed engine values. No native private
// layout is copied; type identity deliberately uses Flax's cached type bridge.
internal static class FlaxAbiConverters
{
    public static void Register(TypeConverterRegistry registry)
    {
        Ticks(registry, "DateTime");
        Ticks(registry, "TimeSpan");
        Type(registry, "VariantType", "UnboxVariantType", "BoxVariantType");
        Type(registry, "ScriptingTypeHandle", "UnboxScriptingTypeHandle", "BoxScriptingTypeHandle");
        Version(registry);
        Culture(registry);
        Color(registry);
        Viewport(registry);
        Frustum(registry);
        registry.RegisterBodyConverter("Char", "unsigned short", new()
        {
            ManagedType = "char", ManagedAbiType = "ushort", CopyReferenceReturns = true, CopiedCallbackValue = true,
            NativeInlineStorage = (2, 2),
            NativeSupport = "static_assert(sizeof(::Char) == 2 && alignof(::Char) == 2, \"Char converter requires two-byte UTF-16 code units and alignment\");",
            ManagedToAbi = c=>new($"(ushort)({c.Value})"), AbiToManaged = c=>new($"(char)({c.Value})"),
            AbiToNative = c=>new($"static_cast<::Char>({c.Value})"), NativeToAbi = c=>new($"static_cast<unsigned short>({c.Value})"),
            ReleaseManagedAbi=_=>"", ReleaseNativeAbi=_=>""
        }, nativeCppType:"::Char",nativeDefault:"static_cast<::Char>(0)",existingManagedType:true);
    }

    static void Ticks(TypeConverterRegistry registry, string type) => registry.RegisterBodyConverter(type, "long long", new()
    {
        ManagedType = "global::System." + type, ManagedAbiType = "long", CopyReferenceReturns = true,
        ManagedToAbi = c => new($"({c.Value}).Ticks"),
        AbiToManaged = c => new($"new global::System.{type}({c.Value})"),
        AbiToNative = c => new($"::{type}({c.Value})"),
        NativeToAbi = c => new($"({c.Value}).Ticks"),
        ReleaseManagedAbi = _ => "", ReleaseNativeAbi = _ => ""
    }, nativeCppType: "::" + type, nativeDefault: "::" + type + "(0)", existingManagedType: true);

    static void Color(TypeConverterRegistry registry) => registry.RegisterBodyConverter("Color32", "unsigned int", new()
    {
        ManagedType = "global::FlaxEngine.Color32", ManagedAbiType = "uint", CopyReferenceReturns = true, InlineCallbackValue = true,
        ManagedToAbi = c => new($"((uint)({c.Value}).R | ((uint)({c.Value}).G << 8) | ((uint)({c.Value}).B << 16) | ((uint)({c.Value}).A << 24))"),
        AbiToManaged = c => new($"new global::FlaxEngine.Color32 {{ R = (byte)({c.Value} & 255u), G = (byte)(({c.Value} >> 8) & 255u), B = (byte)(({c.Value} >> 16) & 255u), A = (byte)(({c.Value} >> 24) & 255u) }}"),
        AbiToNative = c => new($"::Color32(static_cast<unsigned char>({c.Value}), static_cast<unsigned char>({c.Value} >> 8), static_cast<unsigned char>({c.Value} >> 16), static_cast<unsigned char>({c.Value} >> 24))"),
        NativeToAbi = c => new($"(static_cast<unsigned int>(({c.Value}).R) | (static_cast<unsigned int>(({c.Value}).G) << 8) | (static_cast<unsigned int>(({c.Value}).B) << 16) | (static_cast<unsigned int>(({c.Value}).A) << 24))"),
        ReleaseManagedAbi = _ => "", ReleaseNativeAbi = _ => ""
    }, nativeCppType: "::Color32", nativeDefault: "::Color32(0,0,0,0)", existingManagedType: true);

    static void Viewport(TypeConverterRegistry registry)
    {
        string[] fields = ["X", "Y", "Width", "Height", "MinDepth", "MaxDepth"];
        string nativeFields = string.Join(", ", fields.Select(f => "value." + f));
        string managedFields = string.Join(", ", fields.Select(f => "value." + f));
        registry.RegisterBodyConverter("Viewport", "unsigned long long", new()
        {
            ManagedType = "global::FlaxEngine.Viewport", ManagedAbiType = "ulong", CopyReferenceReturns = true,
            NativeRequirements = VETools.Generation.HandleNETcpp.CoTaskMemorySupport,
            ManagedSupport = "internal static class {Support} {\n" +
                "internal static nint Write(global::FlaxEngine.Viewport value) { nint p = global::System.Runtime.InteropServices.Marshal.AllocCoTaskMem(24); try { global::System.Runtime.InteropServices.Marshal.Copy(new float[] { " + managedFields + " }, 0, p, 6); return p; } catch { global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem(p); throw; } }\n" +
                "internal static global::FlaxEngine.Viewport Read(nint p) { if(p == 0) throw new global::System.InvalidOperationException(\"Null Viewport packet\"); float[] v = new float[6]; global::System.Runtime.InteropServices.Marshal.Copy(p,v,0,6); return new global::FlaxEngine.Viewport { " + string.Join(", ", fields.Select((f,i) => f + " = v[" + i + "]")) + " }; }\n}",
            NativeSupport = "namespace {Support} {\nstatic_assert(sizeof(float) == 4, \"Viewport requires 32-bit floats\");\n" +
                "inline ::Viewport Read(unsigned long long p) { if(!p) throw std::invalid_argument(\"Null Viewport packet\"); float v[6]; std::memcpy(v,reinterpret_cast<void*>(p),24); return ::Viewport(v[0],v[1],v[2],v[3],v[4],v[5]); }\n" +
                "inline unsigned long long Write(const ::Viewport& value) { const float v[6] = { " + nativeFields + " }; void* p = ::CoTaskMemAlloc(24); if(!p) throw std::bad_alloc(); std::memcpy(p,v,24); return reinterpret_cast<unsigned long long>(p); }\n}",
            ReleaseManagedAbi = c => $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem((nint)(nuint)({c.Value}));",
            ReleaseNativeAbi = c => $"::CoTaskMemFree(reinterpret_cast<void*>({c.Value}));",
            ManagedToAbi = c =>
            {
                string p = c.Unique("__viewport");
                return new($"(ulong)(nuint){p}", $"nint {p} = 0;", $"{p} = global::{c.Support}.Write({c.Value});", $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem({p});");
            },
            AbiToManaged = c => new($"global::{c.Support}.Read((nint)(nuint)({c.Value}))", Cleanup: $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem((nint)(nuint)({c.Value}));"),
            AbiToNative = c => new($"::{c.Support}::Read({c.Value})"),
            NativeToAbi = c =>
            {
                string p = c.Unique("viewport");
                return new(p, $"unsigned long long {p} = 0;", $"{p} = ::{c.Support}::Write({c.Value});", $"if (!{c.Completed}) ::CoTaskMemFree(reinterpret_cast<void*>({p}));");
            }
        }, nativeCppType: "::Viewport", nativeDefault: "::Viewport(0,0,0,0,0,0)", existingManagedType: true);
    }

    static void Frustum(TypeConverterRegistry registry)
    {
        string[] fields = Enumerable.Range(1, 4).SelectMany(row => Enumerable.Range(1, 4).Select(column => $"M{row}{column}")).ToArray();
        registry.RegisterBodyConverter("BoundingFrustum", "unsigned long long", new()
        {
            ManagedType = "global::FlaxEngine.BoundingFrustum", ManagedAbiType = "ulong", CopyReferenceReturns = true,
            NativeRequirements = VETools.Generation.HandleNETcpp.CoTaskMemorySupport,
            // Planes are derived from the matrix by each implementation. Neither
            // private layout nor the native union's active member crosses the ABI.
            ManagedSupport = "internal static class {Support} {\n" +
                "internal static nint Write(global::FlaxEngine.BoundingFrustum value) { var matrix = value.Matrix; nint p = global::System.Runtime.InteropServices.Marshal.AllocCoTaskMem(64); try { global::System.Runtime.InteropServices.Marshal.Copy(new float[] { " + string.Join(", ", fields.Select(f => "matrix." + f)) + " }, 0, p, 16); return p; } catch { global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem(p); throw; } }\n" +
                "internal static global::FlaxEngine.BoundingFrustum Read(nint p) { if (p == 0) throw new global::System.InvalidOperationException(\"Null BoundingFrustum packet\"); float[] v = new float[16]; global::System.Runtime.InteropServices.Marshal.Copy(p, v, 0, 16); return new global::FlaxEngine.BoundingFrustum(new global::FlaxEngine.Matrix { " + string.Join(", ", fields.Select((f,i) => f + " = v[" + i + "]")) + " }); }\n}",
            NativeSupport = "namespace {Support} {\nstatic_assert(sizeof(float) == 4, \"BoundingFrustum requires 32-bit floats\");\n" +
                "inline ::BoundingFrustum Read(unsigned long long p) { if (!p) throw std::invalid_argument(\"Null BoundingFrustum packet\"); float v[16]; std::memcpy(v, reinterpret_cast<void*>(p), 64); ::Matrix matrix{}; " + string.Join(" ", fields.Select((f,i) => "matrix." + f + " = v[" + i + "];")) + " return ::BoundingFrustum(matrix); }\n" +
                "inline unsigned long long Write(const ::BoundingFrustum& value) { const auto& matrix = value.GetMatrix(); const float v[16] = { " + string.Join(", ", fields.Select(f => "matrix." + f)) + " }; void* p = ::CoTaskMemAlloc(64); if (!p) throw std::bad_alloc(); std::memcpy(p, v, 64); return reinterpret_cast<unsigned long long>(p); }\n}",
            ReleaseManagedAbi = c => $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem((nint)(nuint)({c.Value}));",
            ReleaseNativeAbi = c => $"::CoTaskMemFree(reinterpret_cast<void*>({c.Value}));",
            ManagedToAbi = c =>
            {
                string p = c.Unique("__frustum");
                return new($"(ulong)(nuint){p}", $"nint {p} = 0;", $"{p} = global::{c.Support}.Write({c.Value});", $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem({p});");
            },
            AbiToManaged = c => new($"global::{c.Support}.Read((nint)(nuint)({c.Value}))", Cleanup: $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem((nint)(nuint)({c.Value}));"),
            AbiToNative = c => new($"::{c.Support}::Read({c.Value})"),
            NativeToAbi = c =>
            {
                string p = c.Unique("frustum");
                return new(p, $"unsigned long long {p} = 0;", $"{p} = ::{c.Support}::Write({c.Value});", $"if (!{c.Completed}) ::CoTaskMemFree(reinterpret_cast<void*>({p}));");
            }
        }, nativeCppType: "::BoundingFrustum", nativeDefault: "::BoundingFrustum{}", existingManagedType: true);
    }

    static void Type(TypeConverterRegistry registry, string type, string unbox, string box) => registry.RegisterBodyConverter(type, "unsigned long long", new()
    {
        ManagedType = "global::System.Type", ManagedAbiType = "ulong", CopyReferenceReturns = true,
        NativeRequirements = new() { Includes = ["\"Engine/Scripting/ManagedCLR/MUtils.h\""] },
        ManagedToAbi = c => new($"(ulong)(nuint)global::FlaxEngine.Interop.SystemTypeMarshaller.ConvertToUnmanaged({c.Value})",
            Setup: c.RetainManaged?.Invoke(c.Value) ?? ""),
        AbiToManaged = c => new($"global::FlaxEngine.Interop.SystemTypeMarshaller.ConvertToManaged((nint)(nuint)({c.Value}))"),
        AbiToNative = c => new($"::MUtils::{unbox}(reinterpret_cast<MType*>({c.Value}))"),
        NativeToAbi = c => new($"reinterpret_cast<unsigned long long>(::MUtils::{box}({c.Value}))"),
        // MType/SystemTypeMarshaller use cached handles owned by Flax.
        ReleaseManagedAbi = _ => "", ReleaseNativeAbi = _ => ""
    }, nativeCppType: "::" + type, nativeDefault: "::" + type + "{}", existingManagedType: true);

    static void Culture(TypeConverterRegistry registry)
    {
        if (!registry.TryGet("String", out var text) || text.Bodies is not { } body)
            throw new InvalidOperationException("CultureInfo requires the String converter.");
        registry.RegisterBodyConverter("CultureInfo", text.AbiCppType, new()
        {
            ManagedType = "global::System.Globalization.CultureInfo", ManagedAbiType = body.ManagedAbiType,
            CopyReferenceReturns = true, NativeSupport = body.NativeSupport, ManagedSupport = body.ManagedSupport,
            NativeRequirements = body.NativeRequirements, ReleaseManagedAbi = body.ReleaseManagedAbi, ReleaseNativeAbi = body.ReleaseNativeAbi,
            ManagedToAbi = c => body.ManagedToAbi(c with { Value = $"({c.Value} ?? throw new global::System.ArgumentNullException(\"culture\")).Name" }),
            AbiToManaged = c =>
            {
                var converted = body.AbiToManaged(c);
                return converted with { Expression = $"global::System.Globalization.CultureInfo.GetCultureInfo({converted.Expression})" };
            },
            AbiToNative = c =>
            {
                var converted = body.AbiToNative(c);
                string name = c.Unique("cultureName");
                return converted with { Setup = converted.Setup + $"\nauto {name} = {converted.Expression};", Expression = $"::CultureInfo(::StringView({name}.Get(), {name}.Length()))" };
            },
            NativeToAbi = c => body.NativeToAbi(c with { Value = $"({c.Value}).GetName()" })
        }, nativeCppType: "::CultureInfo", nativeDefault: "::CultureInfo{}", existingManagedType: true);
    }

    static void Version(TypeConverterRegistry registry) => registry.RegisterBodyConverter("Version", "unsigned long long", new()
    {
        ManagedType = "global::System.Version", ManagedAbiType = "ulong", CopyReferenceReturns = true,
        NativeRequirements = VETools.Generation.HandleNETcpp.CoTaskMemorySupport,
        ManagedSupport = VersionManaged, NativeSupport = VersionNative,
        ReleaseManagedAbi = c => $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem((nint)(nuint)({c.Value}));",
        ReleaseNativeAbi = c => $"::CoTaskMemFree(reinterpret_cast<void*>({c.Value}));",
        ManagedToAbi = c =>
        {
            string buffer = c.Unique("__version");
            return new($"(ulong)(nuint){buffer}", $"nint {buffer} = 0;", $"{buffer} = global::{c.Support}.Write({c.Value});", $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem({buffer});");
        },
        AbiToManaged = c => new($"global::{c.Support}.Read((nint)(nuint)({c.Value}))", Cleanup: $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem((nint)(nuint)({c.Value}));"),
        AbiToNative = c => new($"::{c.Support}::Read({c.Value})"),
        NativeToAbi = c =>
        {
            string buffer = c.Unique("version");
            return new(buffer, $"unsigned long long {buffer} = 0;", $"{buffer} = ::{c.Support}::Write({c.Value});", $"if (!{c.Completed}) ::CoTaskMemFree(reinterpret_cast<void*>({buffer}));");
        }
    }, nativeCppType: "::Version", nativeDefault: "::Version{}", existingManagedType: true);

    const string VersionManaged = """
        internal static class {Support} {
        internal static nint Write(global::System.Version value) {
            if (value is null) throw new global::System.ArgumentNullException(nameof(value));
            nint address = global::System.Runtime.InteropServices.Marshal.AllocCoTaskMem(16);
            try { global::System.Runtime.InteropServices.Marshal.Copy(new int[] { value.Major, value.Minor, value.Build, value.Revision }, 0, address, 4); return address; }
            catch { global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem(address); throw; }
        }
        internal static global::System.Version Read(nint address) {
            if (address == 0) throw new global::System.InvalidOperationException("Null Version packet.");
            int[] parts = new int[4]; global::System.Runtime.InteropServices.Marshal.Copy(address, parts, 0, 4);
            if (parts[0] < 0 || parts[1] < 0 || parts[2] < -1 || parts[3] < -1 || parts[2] == -1 && parts[3] != -1)
                throw new global::System.InvalidOperationException("Invalid Version components.");
            return parts[2] == -1 ? new global::System.Version(parts[0], parts[1]) : parts[3] == -1 ? new global::System.Version(parts[0], parts[1], parts[2]) : new global::System.Version(parts[0], parts[1], parts[2], parts[3]);
        }
        }
        """;
    const string VersionNative = """
        namespace {Support} {
        static_assert(sizeof(int) == 4, "Version packet requires 32-bit integers");
        inline ::Version Read(unsigned long long address) {
            if (!address) throw std::invalid_argument("Null Version packet");
            int parts[4]; std::memcpy(parts, reinterpret_cast<const void*>(address), 16);
            if (parts[0] < 0 || parts[1] < 0 || parts[2] < -1 || parts[3] < -1 || (parts[2] == -1 && parts[3] != -1)) throw std::invalid_argument("Invalid Version components");
            if (parts[2] == -1) return ::Version(parts[0], parts[1]);
            if (parts[3] == -1) return ::Version(parts[0], parts[1], parts[2]);
            return ::Version(parts[0], parts[1], parts[2], parts[3]);
        }
        inline unsigned long long Write(const ::Version& value) {
            const int parts[4] = {value.Major(), value.Minor(), value.Build(), value.Revision()};
            void* address = ::CoTaskMemAlloc(16); if (!address) throw std::bad_alloc(); std::memcpy(address, parts, 16); return reinterpret_cast<unsigned long long>(address);
        }
        }
        """;
}
