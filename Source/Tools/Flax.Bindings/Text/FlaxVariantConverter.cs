namespace VETools.Text;

// Explicit value serialization. No CLR object pointers, guessed struct layouts,
// or aliases into Variant-owned buffers cross the ABI.
internal static class FlaxVariantConverter
{
    public static void Register(TypeConverterRegistry registry) => registry.RegisterBodyConverter("Variant", "unsigned long long", new()
    {
        MoveInputs = true, ManagedType = "object", ManagedAbiType = "ulong", CopyReferenceReturns = true,
        ManagedSupport = ManagedHelpers, NativeSupport = NativeHelpers,
        NativeRequirements = NativeSupportRequirements.Merge([VETools.Generation.HandleNETcpp.CoTaskMemorySupport, new() { Includes = ["<vector>", "\"Engine/Core/Types/Variant.h\"", "\"Engine/Core/Types/Guid.h\"", "\"Engine/Core/Types/StringView.h\"", "\"Engine/Core/Types/Span.h\"", "\"Engine/Core/Collections/Array.h\"", "\"Engine/Core/Collections/Dictionary.h\""] }]),
        ReleaseManagedAbi = c => $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem((nint)(nuint)({c.Value}));",
        ReleaseNativeAbi = c => $"::CoTaskMemFree(reinterpret_cast<void*>({c.Value}));",
        ManagedToAbi = c =>
        {
            string buffer = c.Unique("__variantBuffer");
            return new($"(ulong)(nuint){buffer}", $"nint {buffer} = 0;", $"{buffer} = global::{c.Support}.Write({c.Value});",
                $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem({buffer});");
        },
        AbiToManaged = c => new($"global::{c.Support}.Read((nint)(nuint)({c.Value}))", Cleanup: $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem((nint)(nuint)({c.Value}));"),
        AbiToNative = c => new($"::{c.Support}::Read({c.Value})"),
        NativeToAbi = c =>
        {
            string buffer = c.Unique("variantBuffer");
            return new(buffer, $"unsigned long long {buffer} = 0;", $"{buffer} = ::{c.Support}::Write({c.Value});",
                $"if (!{c.Completed}) ::CoTaskMemFree(reinterpret_cast<void*>({buffer}));");
        }
    }, nativeCppType: "::Variant", nativeDefault: "::Variant{}", existingManagedType: true);

    // Wire: int32 payload size, then tagged values. All integers are little endian.
    // Tags 0..15: null, bool, short, ushort, int, uint, long, ulong,
    // float, double, UTF16 string, Guid words, blob, array, dictionary, pointer.
    const string ManagedHelpers = """
        internal static class {Support} {
            const int MaxBytes = 16 * 1024 * 1024, MaxItems = 65536, MaxDepth = 32;
            sealed class BoundedStream : global::System.IO.MemoryStream {
                void Check(int count) { if (count < 0 || Position > MaxBytes - count) throw new global::System.NotSupportedException("Variant packet exceeds 16 MiB."); }
                public override void Write(byte[] buffer, int offset, int count) { Check(count); base.Write(buffer, offset, count); }
                public override void Write(global::System.ReadOnlySpan<byte> buffer) { Check(buffer.Length); base.Write(buffer); }
                public override void WriteByte(byte value) { Check(1); base.WriteByte(value); }
            }
            internal static nint Write(object value) {
                using var stream = new BoundedStream();
                using var writer = new global::System.IO.BinaryWriter(stream, global::System.Text.Encoding.Unicode, true);
                Put(writer, value, 0); writer.Flush(); byte[] data = stream.ToArray();
                nint buffer = global::System.Runtime.InteropServices.Marshal.AllocCoTaskMem(checked(4 + data.Length));
                try { global::System.Runtime.InteropServices.Marshal.WriteInt32(buffer, data.Length); global::System.Runtime.InteropServices.Marshal.Copy(data, 0, buffer + 4, data.Length); return buffer; }
                catch { global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem(buffer); throw; }
            }
            static void Count(int count) { if (count < 0 || count > MaxItems) throw new global::System.NotSupportedException("Variant collection exceeds 65536 items."); }
            static void Put(global::System.IO.BinaryWriter w, object value, int depth) {
                if (depth > MaxDepth) throw new global::System.NotSupportedException("Variant nesting exceeds 32 (possibly a cycle).");
                switch (value) {
                case null: w.Write((byte)0); break;
                case bool v: w.Write((byte)1); w.Write(v); break;
                case short v: w.Write((byte)2); w.Write(v); break;
                case ushort v: w.Write((byte)3); w.Write(v); break;
                case int v: w.Write((byte)4); w.Write(v); break;
                case uint v: w.Write((byte)5); w.Write(v); break;
                case long v: w.Write((byte)6); w.Write(v); break;
                case ulong v: w.Write((byte)7); w.Write(v); break;
                case float v: w.Write((byte)8); w.Write(v); break;
                case double v: w.Write((byte)9); w.Write(v); break;
                case sbyte v: Put(w, (int)v, depth); break;
                case byte v: Put(w, (int)v, depth); break;
                case char v: Put(w, (ushort)v, depth); break;
                case string v:
                    if (v.Length > (MaxBytes - 5) / 2) throw new global::System.NotSupportedException("Variant string exceeds packet limit.");
                    w.Write((byte)10); w.Write(v.Length); foreach (char c in v) w.Write((ushort)c); break;
                case global::System.Guid v: w.Write((byte)11); w.Write(v.ToByteArray()); break;
                case byte[] v:
                    if (v.Length > MaxBytes - 5) throw new global::System.NotSupportedException("Variant blob exceeds packet limit.");
                    w.Write((byte)12); w.Write(v.Length); w.Write(v); break;
                case global::System.Array v when v.Rank == 1:
                    Count(v.Length); w.Write((byte)13); w.Write(v.Length); foreach (object element in v) Put(w, element, depth + 1); break;
                case global::System.Collections.IDictionary v:
                    Count(v.Count); w.Write((byte)14); w.Write(v.Count);
                    foreach (global::System.Collections.DictionaryEntry pair in v) { if (pair.Key == null) throw new global::System.NotSupportedException("Null Variant dictionary key."); Put(w, pair.Key, depth + 1); Put(w, pair.Value, depth + 1); } break;
                case global::System.IntPtr v: w.Write((byte)15); w.Write((ulong)(nuint)v); break;
                default: throw new global::System.NotSupportedException("No Variant value converter registered for managed type '" + value.GetType().FullName + "'.");
                }
            }
            internal static object Read(nint buffer) {
                if (buffer == 0) throw new global::System.InvalidOperationException("Null Variant packet.");
                int size = global::System.Runtime.InteropServices.Marshal.ReadInt32(buffer);
                if (size < 1 || size > MaxBytes) throw new global::System.InvalidOperationException("Invalid Variant packet size.");
                byte[] bytes = new byte[size]; global::System.Runtime.InteropServices.Marshal.Copy(buffer + 4, bytes, 0, size);
                using var stream = new global::System.IO.MemoryStream(bytes, false); using var reader = new global::System.IO.BinaryReader(stream);
                object value = Get(reader, 0); if (stream.Position != size) throw new global::System.InvalidOperationException("Trailing Variant payload."); return value;
            }
            static int Length(global::System.IO.BinaryReader r, int width, int max) {
                int count = r.ReadInt32(); if (count < 0 || count > max || (long)count * width > r.BaseStream.Length - r.BaseStream.Position) throw new global::System.InvalidOperationException("Invalid Variant length."); return count;
            }
            static byte[] Bytes(global::System.IO.BinaryReader r, int size) { byte[] data = r.ReadBytes(size); if (data.Length != size) throw new global::System.IO.EndOfStreamException(); return data; }
            static object Get(global::System.IO.BinaryReader r, int depth) {
                if (depth > MaxDepth) throw new global::System.InvalidOperationException("Variant nesting exceeds 32.");
                switch (r.ReadByte()) {
                case 0: return null;
                case 1: { byte b = r.ReadByte(); if (b > 1) throw new global::System.InvalidOperationException("Invalid Variant bool."); return b != 0; }
                case 2: return r.ReadInt16(); case 3: return r.ReadUInt16(); case 4: return r.ReadInt32(); case 5: return r.ReadUInt32();
                case 6: return r.ReadInt64(); case 7: return r.ReadUInt64(); case 8: return r.ReadSingle(); case 9: return r.ReadDouble();
                case 10: { int n = Length(r, 2, MaxBytes / 2); char[] chars = new char[n]; for (int i = 0; i < n; ++i) chars[i] = (char)r.ReadUInt16(); return new string(chars); }
                case 11: return new global::System.Guid(Bytes(r, 16));
                case 12: return Bytes(r, Length(r, 1, MaxBytes));
                case 13: { int n = Length(r, 1, MaxItems); object[] values = new object[n]; for (int i = 0; i < n; ++i) values[i] = Get(r, depth + 1); return values; }
                case 14: { int n = Length(r, 2, MaxItems); var values = new global::System.Collections.Generic.Dictionary<object, object>(); for (int i = 0; i < n; ++i) { object key = Get(r, depth + 1); if (key == null || values.ContainsKey(key)) throw new global::System.InvalidOperationException("Null or duplicate Variant dictionary key."); values.Add(key, Get(r, depth + 1)); } return values; }
                case 15: return (nint)(nuint)r.ReadUInt64();
                default: throw new global::System.NotSupportedException("Unknown Variant wire tag.");
                }
            }
        }
        """;

    const string NativeHelpers = """
        namespace {Support} {
        constexpr int MaxBytes = 16 * 1024 * 1024, MaxItems = 65536, MaxDepth = 32;
        struct Reader {
            const unsigned char* data; int left;
            template<typename T> T Get() { if (left < static_cast<int>(sizeof(T))) throw std::invalid_argument("Truncated Variant packet."); T value; std::memcpy(&value, data, sizeof(T)); data += sizeof(T); left -= sizeof(T); return value; }
            int Length(int width, int max) { int n = Get<int>(); if (n < 0 || n > max || n > left / width) throw std::invalid_argument("Invalid Variant length."); return n; }
        };
        struct Writer {
            std::vector<unsigned char> data;
            void Bytes(const void* value, int size) { if (size < 0 || size > MaxBytes - static_cast<int>(data.size()) || (size && !value)) throw std::invalid_argument("Variant packet exceeds limit or has null data."); if (size) { auto* p = static_cast<const unsigned char*>(value); data.insert(data.end(), p, p + size); } }
            template<typename T> void Put(T value) { Bytes(&value, sizeof(T)); }
            void Tag(unsigned char tag) { Put(tag); }
        };
        inline ::Variant Decode(Reader& r, int depth) {
            if (depth > MaxDepth) throw std::invalid_argument("Variant nesting exceeds 32.");
            switch (r.Get<unsigned char>()) {
            case 0: return ::Variant{};
            case 1: { auto b = r.Get<unsigned char>(); if (b > 1) throw std::invalid_argument("Invalid Variant bool."); return ::Variant(b != 0); }
            case 2: return ::Variant(r.Get<int16>()); case 3: return ::Variant(r.Get<uint16>());
            case 4: return ::Variant(r.Get<int32>()); case 5: return ::Variant(r.Get<uint32>());
            case 6: return ::Variant(r.Get<int64>()); case 7: return ::Variant(r.Get<uint64>());
            case 8: return ::Variant(r.Get<float>()); case 9: return ::Variant(r.Get<double>());
            case 10: { int n = r.Length(2, MaxBytes / 2); ::String text; text.Resize(n); for (int i = 0; i < n; ++i) text.Get()[i] = static_cast<Char>(r.Get<uint16>()); return ::Variant(::StringView(text)); }
            case 11: { ::Guid value; value.A = r.Get<uint32>(); value.B = r.Get<uint32>(); value.C = r.Get<uint32>(); value.D = r.Get<uint32>(); return ::Variant(value); }
            case 12: { int n = r.Length(1, MaxBytes); ::Variant value(::Span<byte>(reinterpret_cast<byte*>(const_cast<unsigned char*>(r.data)), n)); r.data += n; r.left -= n; return value; }
            case 13: { int n = r.Length(1, MaxItems); ::Array<::Variant> values; values.Resize(n); for (int i = 0; i < n; ++i) values[i] = Decode(r, depth + 1); return ::Variant(values); }
            case 14: { int n = r.Length(2, MaxItems); ::Dictionary<::Variant, ::Variant> values; for (int i = 0; i < n; ++i) { auto key = Decode(r, depth + 1); if (key.Type.Type == ::VariantType::Null || values.ContainsKey(key)) throw std::invalid_argument("Null or duplicate Variant dictionary key."); auto value = Decode(r, depth + 1); values.Add(key, value); } return ::Variant(values); }
            case 15: return ::Variant(reinterpret_cast<void*>(r.Get<uint64>()));
            default: throw std::invalid_argument("Unknown Variant wire tag.");
            }
        }
        inline void Encode(Writer& w, const ::Variant& value, int depth) {
            if (depth > MaxDepth) throw std::invalid_argument("Variant nesting exceeds 32.");
            switch (value.Type.Type) {
            case ::VariantType::Null: w.Tag(0); break;
            case ::VariantType::Bool: w.Tag(1); w.Put<unsigned char>(value.AsBool ? 1 : 0); break;
            case ::VariantType::Int16: w.Tag(2); w.Put<int16>(value.AsInt16); break;
            case ::VariantType::Uint16: w.Tag(3); w.Put<uint16>(value.AsUint16); break;
            case ::VariantType::Int: w.Tag(4); w.Put<int32>(value.AsInt); break;
            case ::VariantType::Uint: w.Tag(5); w.Put<uint32>(value.AsUint); break;
            case ::VariantType::Int64: w.Tag(6); w.Put<int64>(value.AsInt64); break;
            case ::VariantType::Uint64: w.Tag(7); w.Put<uint64>(value.AsUint64); break;
            case ::VariantType::Float: w.Tag(8); w.Put<float>(value.AsFloat); break;
            case ::VariantType::Double: w.Tag(9); w.Put<double>(value.AsDouble); break;
            case ::VariantType::String: { auto text = static_cast<::StringView>(value); int n = text.Length(); if (n < 0 || n > (MaxBytes - 5) / 2 || (n && !text.Get())) throw std::invalid_argument("Invalid Variant string."); w.Tag(10); w.Put<int>(n); for (int i = 0; i < n; ++i) w.Put<uint16>(text.Get()[i]); break; }
            case ::VariantType::Guid: { const auto& guid = value.AsGuid(); w.Tag(11); w.Put<uint32>(guid.A); w.Put<uint32>(guid.B); w.Put<uint32>(guid.C); w.Put<uint32>(guid.D); break; }
            case ::VariantType::Blob: { int n = value.AsBlob.Length; if (n < 0 || n > MaxBytes - 5) throw std::invalid_argument("Invalid Variant blob."); w.Tag(12); w.Put<int>(n); w.Bytes(value.AsBlob.Data, n); break; }
            case ::VariantType::Array: { const auto& values = value.AsArray(); int n = values.Count(); if (n < 0 || n > MaxItems) throw std::invalid_argument("Variant array exceeds limit."); w.Tag(13); w.Put<int>(n); for (int i = 0; i < n; ++i) Encode(w, values[i], depth + 1); break; }
            case ::VariantType::Dictionary: { auto* values = value.AsDictionary; if (!values) throw std::invalid_argument("Null Variant dictionary."); int n = values->Count(); if (n < 0 || n > MaxItems) throw std::invalid_argument("Variant dictionary exceeds limit."); w.Tag(14); w.Put<int>(n); for (const auto& pair : *values) { if (pair.Key.Type.Type == ::VariantType::Null) throw std::invalid_argument("Null Variant dictionary key."); Encode(w, pair.Key, depth + 1); Encode(w, pair.Value, depth + 1); } break; }
            case ::VariantType::Pointer: w.Tag(15); w.Put<uint64>(reinterpret_cast<uint64>(value.AsPointer)); break;
            default: throw std::invalid_argument("No Variant wire converter for this native tag (object, enum, math, structure or managed object).");
            }
        }
        inline ::Variant Read(unsigned long long address) {
            if (!address) throw std::invalid_argument("Null Variant packet.");
            auto* p = reinterpret_cast<const unsigned char*>(address); int size; std::memcpy(&size, p, 4);
            if (size < 1 || size > MaxBytes) throw std::invalid_argument("Invalid Variant packet size.");
            Reader reader{p + 4, size}; auto value = Decode(reader, 0); if (reader.left != 0) throw std::invalid_argument("Trailing Variant payload."); return value;
        }
        inline unsigned long long Write(const ::Variant& value) {
            Writer writer; Encode(writer, value, 0); int size = static_cast<int>(writer.data.size());
            auto* packet = static_cast<unsigned char*>(::CoTaskMemAlloc(4 + size)); if (!packet) throw std::bad_alloc();
            std::memcpy(packet, &size, 4); std::memcpy(packet + 4, writer.data.data(), size); return reinterpret_cast<unsigned long long>(packet);
        }
        }
        """;
}
