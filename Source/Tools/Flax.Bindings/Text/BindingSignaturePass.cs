using static VETools.Tokenizer;

namespace VETools.Text;

internal enum ParameterDirection { In, Ref, Out }
internal enum ParameterPassing { Value, PointerValue, LValueReference, RValueReference }
internal enum AccessStorage { LocalCopy, CallerAlias, Pointee }
internal sealed record ParameterAccess(int Level, AccessStorage Storage, TypeQualifiers Qualifiers, bool CanRead, bool CanWrite);

internal sealed class ParameterBindingInfo
{
    public required TypeInfo EffectiveType;
    public ParameterPassing Passing;
    public ParameterDirection Direction;
    public ParameterDirection? NameOverride;
    public bool ReadOnlyReference;
    public List<ParameterAccess> Access = [];
    public bool RequiresInitialValue => Direction != ParameterDirection.Out;
}

internal sealed class FunctionBindingInfo
{
    public string? NativeSignature;
    public FunctionDescriptor? RedeclarationOf;
    public List<ParameterBindingInfo> Parameters = [];
}

internal sealed class BindingAnalysisResult
{
    public List<LogEntry> Diagnostics = [];
    public IEnumerable<LogEntry> Errors => Diagnostics.Where(entry => entry.IsError);
    public IEnumerable<LogEntry> Warnings => Diagnostics.Where(entry => entry.IsWarning && !entry.IsError);
    public bool HasErrors => Errors.Any();
    public bool HasBlockingErrors => Diagnostics.Any(entry => entry.BlocksGeneration);
    public Dictionary<string, FunctionDescriptor> Functions = new(StringComparer.Ordinal);
}

internal static class BindingSignaturePass
{
    public static BindingAnalysisResult Run(FileDescriptor file, Func<FunctionDescriptor, bool>? include = null)
    {
        if (file.TypeInfos == null)
        {
            TypeInfoPass.Run(file);
            if (include == null) return file.Bindings!;
        }
        BindingAnalysisResult result = new();
        var families = new Dictionary<string, List<FunctionDescriptor>>(StringComparer.Ordinal);
        foreach (var function in Functions(file).Concat(file.TypeInfos!.TemplateInstances.Values.Where(t => t.SourceFile == file).SelectMany(Functions)).Distinct())
        {
            if (include != null && !include(function)) continue;
            // A template prototype is not a callable overload until specialized.
            if (function.IsTemplate) continue;
            FunctionBindingInfo binding = new();
            function.Binding = binding;
            if (function.TypeInfo == null) continue;
            foreach (var parameter in function.Fields)
            {
                parameter.Binding = Analyze(parameter, result);
                if (parameter.Binding != null) binding.Parameters.Add(parameter.Binding);
            }
            var parameters = function.TypeInfo.Parameters.Select(t => Canonical(t, true)).ToList();
            if (parameters.Any(p => p == null)) continue; // No guessed aliases/dependent type identities.
            string family = function.QualifiedName + "(" + string.Join(",", parameters) + ")";
            string signature = family + ":cv=" + (int)function.TypeInfo.Qualifiers + ":ref=" + function.TypeInfo.ReferenceQualifier;
            binding.NativeSignature = signature;
            if (!families.TryGetValue(family, out var siblings)) families.Add(family, siblings = []);
            foreach (var sibling in siblings)
            {
                if (function.Parent is not TypeDescriptor || sibling.TypeInfo == null) continue;
                bool eitherStatic = IsStatic(function) || IsStatic(sibling);
                bool refMismatch = (function.TypeInfo.ReferenceQualifier == null) != (sibling.TypeInfo.ReferenceQualifier == null);
                if (eitherStatic && (IsStatic(function) != IsStatic(sibling) || signature != sibling.Binding?.NativeSignature))
                    Error(function, "Static/non-static members cannot overload the same native parameter list.", result);
                if (refMismatch)
                    Error(function, "Cannot mix ref-qualified and unqualified members with the same native parameter list.", result);
            }
            siblings.Add(function);
            if (!result.Functions.TryGetValue(signature, out var previous))
            { result.Functions.Add(signature, function); continue; }
            bool conflict = false;
            for (int i = 0; i < binding.Parameters.Count && i < previous.Binding!.Parameters.Count; i++)
            {
                if (binding.Parameters[i].Direction == previous.Binding.Parameters[i].Direction) continue;
                var parameter = function.Fields[i];
                result.Diagnostics.Add(new LogEntry($"Conflicting binding direction for the same native signature '{function.QualifiedName}', parameter {i + 1}: {previous.Binding.Parameters[i].Direction} versus {binding.Parameters[i].Direction}. Previous declaration is at {previous.Name.line + 1}:{previous.Name.col + 1}. Parameter names do not create overloads.", parameter.Name));
                conflict = true;
            }
            string? oldReturn = Canonical(previous.TypeInfo?.ReturnType, false);
            string? newReturn = Canonical(function.TypeInfo.ReturnType, false);
            if (oldReturn != null && newReturn != null && oldReturn != newReturn)
            { Error(function, "Native functions cannot overload by return type alone.", result); conflict = true; }
            if (function.Parent is TypeDescriptor)
            { Error(function, "Duplicate native member signature; parameter names/directions do not create an overload.", result); conflict = true; }
            else if (Definition(function) && Definition(previous))
            { Error(function, "Multiple definitions of the same native function.", result); conflict = true; }
            if (conflict) continue;
            // Legal free-function redeclarations are emitted once, preferring the definition.
            if (Definition(function) && !Definition(previous))
            {
                previous.Binding!.RedeclarationOf = function;
                result.Functions[signature] = function;
            }
            else binding.RedeclarationOf = previous;
        }
        file.Bindings = result;
        return result;
    }

    static IEnumerable<FunctionDescriptor> Functions(ScopeDescriptor scope)
    {
        foreach (var declaration in scope.Declarations)
        {
            if (declaration is FunctionDescriptor function) yield return function;
            if (declaration is ScopeDescriptor child)
                foreach (var functionInChild in Functions(child)) yield return functionInChild;
        }
    }

    static ParameterBindingInfo? Analyze(FieldDescriptor parameter, BindingAnalysisResult result)
    {
        if (parameter.TypeInfo == null) return null;
        TypeInfo effective = Adjust(parameter.TypeInfo);
        ParameterBindingInfo info = new() { EffectiveType = effective };
        TypeInfo storage = effective;
        AccessStorage origin = AccessStorage.LocalCopy;
        switch (effective)
        {
            case ReferenceTypeInfo reference:
                info.Passing = reference.ReferenceKind == ReferenceKind.LValue ? ParameterPassing.LValueReference : ParameterPassing.RValueReference;
                storage = reference.Referent;
                origin = AccessStorage.CallerAlias;
                info.Direction = Const(storage) ? ParameterDirection.In : ParameterDirection.Ref;
                if (reference.ReferenceKind == ReferenceKind.RValue && storage is NamedTypeInfo { Converter.Bodies.MoveInputs: true })
                { info.Direction = ParameterDirection.In; origin = AccessStorage.LocalCopy; }
                break;
            case PointerTypeInfo pointer:
                info.Passing = ParameterPassing.PointerValue;
                info.Direction = Const(pointer.Pointee) ? ParameterDirection.In : ParameterDirection.Ref;
                break;
            default:
                info.Passing = ParameterPassing.Value;
                info.Direction = ParameterDirection.In;
                break;
        }
        int level = 0;
        while (true)
        {
            info.Access.Add(new(level++, origin, Qualifiers(storage), true, !Const(storage)));
            if (storage is not PointerTypeInfo pointer) break;
            storage = pointer.Pointee;
            origin = AccessStorage.Pointee;
        }
        info.NameOverride = parameter.Api.Direction ?? Prefix(parameter.Name.text);
        if (info.NameOverride is { } direction)
        {
            bool writableReference = info.Passing == ParameterPassing.LValueReference && effective is ReferenceTypeInfo reference && !Const(reference.Referent);
            string source = parameter.Api.Direction != null ? $"API_PARAM({direction})" : $"{direction} prefix";
            if (direction == ParameterDirection.Out && !writableReference)
                result.Diagnostics.Add(new LogEntry(source + " requires a writable C++ lvalue reference (&), including a writable pointer reference (*&).", parameter.Name));
            else if (direction == ParameterDirection.Ref && info.Passing == ParameterPassing.LValueReference && effective is ReferenceTypeInfo readOnly && Const(readOnly.Referent))
            { info.Direction = ParameterDirection.In; info.ReadOnlyReference = true; }
            else if (direction == ParameterDirection.Ref && info.Direction != ParameterDirection.Ref)
                result.Diagnostics.Add(new LogEntry($"{source} contradicts the native access contract for '{parameter.TypeInfo?.Display} {parameter.Name.text}'. Ref requires a compatible native reference or writable pointer target; this parameter does not provide one. The C++ declaration itself may be valid. Remove the Ref override or use In for read-only input; use a writable reference only if the native API is intended to modify that storage.", parameter.Name));
            else info.Direction = direction;
        }
        return info;
    }

    public static ParameterDirection? Prefix(string? name)
    {
        if (string.IsNullOrEmpty(name)) return null;
        foreach (var direction in Enum.GetValues<ParameterDirection>())
        {
            string prefix = direction.ToString();
            if (!name.StartsWith(prefix, StringComparison.Ordinal) || name.Length <= prefix.Length) continue;
            char next = name[prefix.Length];
            if (char.IsUpper(next) || next == '_' && name.Length > prefix.Length + 1) return direction;
        }
        return null;
    }

    static TypeInfo Adjust(TypeInfo type) => type is ArrayTypeInfo array ? new PointerTypeInfo { Pointee = array.ElementType } : type;
    static TypeQualifiers Qualifiers(TypeInfo type) => type switch
    {
        NamedTypeInfo named => named.EffectiveQualifiers,
        PointerTypeInfo pointer => pointer.Qualifiers,
        FunctionPointerTypeInfo function => function.Qualifiers,
        ArrayTypeInfo array => Qualifiers(array.ElementType),
        _ => TypeQualifiers.None
    };
    static bool Const(TypeInfo type) => (Qualifiers(type) & TypeQualifiers.Const) != 0;
    public static bool IsStatic(FunctionDescriptor function)
    {
        if (function.GeneratedStatic) return true;
        if (function.NameTokens.Count == 0) return false;
        var name = function.NameTokens[0];
        return function.HeaderTokens.TakeWhile(t => t.line != name.line || t.col != name.col).Any(t => t == "static");
    }
    static bool Definition(FunctionDescriptor function) => !function.IsDeclaration || function.TypeInfo?.SuffixTokens.Any(t => t.text is "default" or "delete") == true;
    static void Error(FunctionDescriptor function, string message, BindingAnalysisResult result) => result.Diagnostics.Add(new LogEntry(message, function.Name));

    public static string? Canonical(TypeInfo? original, bool parameter)
    {
        if (original == null) return null;
        TypeInfo type = parameter ? Adjust(original) : original;
        string Cv(TypeQualifiers qualifiers) => parameter ? "" : ":cv=" + (int)qualifiers;
        switch (type)
        {
            case NamedTypeInfo named:
                if(named.Converter?.NativeSignatureIdentity is { } identity) return identity + Cv(named.EffectiveQualifiers);
                if (named.Symbol?.Definition is TypeDescriptor { ConcreteTemplate: { } concrete })
                    return "template:" + concrete.Definition.QualifiedName + "<" + string.Join(",", concrete.Definition.TemplateParameters.Select(p => Canonical(concrete.TypeArguments[p.Name.text], false))) + ">" + Cv(named.EffectiveQualifiers);
                if (named.Converter is { AutomaticFields: false, EnumValue: false } converter)
                    return converter.IsAlias ? Canonical(converter.AbiType(named.EffectiveQualifiers), parameter) : "converted:" + converter.NativeCppType + Cv(named.EffectiveQualifiers);
                if (named.Arguments.Count != 0)
                {
                    if (named.Resolution != TypeResolution.Resolved || named.Specialization == null) return null;
                    var templateArguments = named.Arguments.Select(argument => Canonical(argument.Type, false)).ToArray();
                    if (templateArguments.Any(argument => argument == null)) return null;
                    return "template:" + named.Specialization.Definition.QualifiedName + "<" + string.Join(",", templateArguments) + ">" + Cv(named.EffectiveQualifiers);
                }
                if (named.Resolution == TypeResolution.Builtin) return "builtin:" + Builtin(named.Name) + Cv(named.Qualifiers);
                if (named.Resolution == TypeResolution.Resolved && named.Symbol != null) return "type:" + named.Symbol.QualifiedName + Cv(named.Qualifiers);
                return null;
            case PointerTypeInfo pointer:
                var pointee = Canonical(pointer.Pointee, false);
                return pointee == null ? null : "ptr(" + pointee + ")" + Cv(pointer.Qualifiers);
            case ReferenceTypeInfo reference:
                var referent = Canonical(reference.Referent, false);
                return referent == null ? null : (reference.ReferenceKind == ReferenceKind.LValue ? "lref(" : "rref(") + referent + ")";
            case ArrayTypeInfo array:
                var element = Canonical(array.ElementType, false);
                return element == null ? null : "array[" + string.Concat(array.EffectiveExtentTokens.Where(t => !t.IsTrivia).Select(t => t.text)) + "](" + element + ")";
            case FunctionPointerTypeInfo function:
                var returns = Canonical(function.ReturnType, false);
                var arguments = function.Parameters.Select(p => Canonical(p, true)).ToList();
                return returns == null || arguments.Any(a => a == null) ? null : "fnptr(" + returns + ";" + string.Join(",", arguments) + ")" + Cv(function.Qualifiers);
            default: return null;
        }
    }

    static string Builtin(string name)
    {
        var words = name.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        bool unsigned = words.Contains("unsigned");
        foreach (string distinct in new[] { "void", "bool", "char8_t", "char16_t", "char32_t", "wchar_t", "float" })
            if (words.Contains(distinct)) return distinct;
        if (words.Contains("char")) return unsigned ? "unsigned char" : words.Contains("signed") ? "signed char" : "char";
        int longs = words.Count(w => w == "long");
        if (words.Contains("double")) return longs > 0 ? "long double" : "double";
        string core = words.Contains("short") ? "short" : longs == 2 ? "long long" : longs == 1 ? "long" : "int";
        return (unsigned ? "unsigned " : "") + core;
    }
}
