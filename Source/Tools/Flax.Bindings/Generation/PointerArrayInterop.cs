using VETools.Text;
using TypeInfo = VETools.Text.TypeInfo;

namespace VETools.Generation;

internal static class PointerArrayInterop
{
    internal sealed record Description(ClassDescriptor Element, int CountIndex, string CountName);
    public static bool Configured(FieldDescriptor field) => field.Parent is FunctionDescriptor function &&
        field.SourceFile?.Converters.InputPointerArrayCount(function.QualifiedName, field.Name.text) != null;
    public static Description? Describe(FieldDescriptor field)
    {
        if (field.Parent is not FunctionDescriptor function ||
            field.SourceFile?.Converters.InputPointerArrayCount(function.QualifiedName, field.Name.text) is not { } count) return null;
        if (field.TypeInfo is not PointerTypeInfo { Qualifiers: TypeQualifiers.None,
            Pointee: PointerTypeInfo { Qualifiers: TypeQualifiers.None, Pointee: NamedTypeInfo { EffectiveQualifiers: TypeQualifiers.None } } element } ||
            HandleNETcpp.Use(element) is not { Pointer: true, Slot: false } use || use.Type is not ClassDescriptor elementClass || !HandleNETcpp.CanCast(use.Type)) return null;
        int index = function.Fields.FindIndex(f => f.Name.text == count);
        if (index < 0 || function.Fields[index].TypeInfo is not NamedTypeInfo countType || ScalarAbi.Resolve(countType)?.CS != "int") return null;
        if (field.Api.Direction is ParameterDirection.Out or ParameterDirection.Ref) return null;
        return new(elementClass, index, count);
    }
    public static bool EmitManaged(EmittedParameter parameter, IReadOnlyList<EmittedParameter> parameters, Func<string,string> unique,
        List<string> init, List<string> setup, List<string> fixedLines, List<string> imports, List<string> args, ICollection<string> keepAlive)
    {
        if (parameter.InputPointerArray is not { } info) return false;
        string value = parameter.ManagedName, count = parameters[info.CountIndex].ManagedName;
        string buffer = unique("__actorHandles"), pointer = unique("__actors");
        init.Add("nint[] " + buffer + " = null;");
        setup.Add("if (" + count + " < 0 || " + count + " > (" + value + "?.Length ?? 0)) throw new global::System.ArgumentOutOfRangeException(nameof(" + count + "));");
        setup.Add(buffer + " = new nint[" + count + "];");
        string index = unique("__actorIndex");
        setup.Add("for (int " + index + " = 0; " + index + " < " + count + "; ++" + index + ") " + buffer + "[" + index + "] = " + HandleNetCS.ToNative(value + "[" + index + "]") + ";");
        fixedLines.Add("fixed (nint* " + pointer + " = " + buffer + ")");
        imports.Add("nint* " + unique("__arg")); imports.Add("int " + unique("__length"));
        args.Add(pointer); args.Add(buffer + ".Length"); keepAlive.Add("global::System.GC.KeepAlive(" + value + ");"); return true;
    }
    public static void EmitNative(EmittedParameter parameter, string arg, List<string> parameters, List<string> args, List<string> checks, List<string> setup)
    {
        var info = parameter.InputPointerArray!; string count = "arg" + info.CountIndex, length = arg + "Length", local = arg + "Actors";
        parameters.Add(HandleNETcpp.RootName(info.Element) + "** " + arg); parameters.Add("int " + length);
        checks.Add(count + " < 0 || " + length + " < 0 || " + count + " > " + length + " || (" + count + " > 0 && !" + arg + ")");
        checks.Add(arg + " && reinterpret_cast<std::uintptr_t>(" + arg + ") % alignof(void*) != 0");
        string type = "::" + info.Element.NativeQualifiedName;
        setup.Add("std::vector<" + type + "*> " + local + "; " + local + ".reserve(" + count + ");");
        setup.Add("for (int i = 0; i < " + count + "; ++i) " + local + ".push_back(static_cast<" + type + "*>(" + arg + "[i]));");
        args.Add(local + ".empty() ? nullptr : " + local + ".data()");
    }
}
