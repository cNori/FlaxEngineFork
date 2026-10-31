using VETools.Text;

namespace VETools.Generation;

// Recover only known unsupported binding contracts. Syntax, overload identity,
// configuration and generator consistency errors remain fatal.
internal static class BindingGeneration
{
    public sealed record Result(CSharpGenerationResult Managed, CppGenerationResult? Native, ManagedInteropResult? Interop);

    public static Result Generate(FileDescriptor file, string abi, string? nativeHeader = null,
        NativeEntryPointNaming naming = NativeEntryPointNaming.Readable, string? version = null, string library = "FlaxEngine", bool separateSharedDeclarations = false)
    {
        if (file.TypeInfos == null) TypeInfoPass.Run(file);
        var plan = ExportPlan.Build(file);
        List<LogEntry> exclusions = [];
        while (true)
        {
            var managed = CSharpGenerator.Generate(file, abi, plan, separateSharedDeclarations);
            if (!managed.Success)
            {
                if (Recover(managed.Diagnostics)) continue;
                return Finish(managed, null, null);
            }
            if (nativeHeader == null) return Finish(managed, null, null);
            var native = CppGenerator.Generate(managed, nativeHeader, naming, version);
            if (!native.Success)
            {
                if (Recover(native.Diagnostics)) continue;
                return Finish(managed, native, null);
            }
            var interop = ManagedInteropGenerator.Generate(managed, native, library);
            if (!interop.Success && Recover(interop.Diagnostics)) continue;
            return Finish(managed, native, interop);
        }

        Result Finish(CSharpGenerationResult managed, CppGenerationResult? native, ManagedInteropResult? interop)
        {
            managed.Diagnostics.InsertRange(0, exclusions);
            return new(managed, native, interop);
        }

        bool Recover(IEnumerable<LogEntry> diagnostics)
        {
            bool progress = false;
            foreach (var diagnostic in diagnostics.Where(d => d.BlocksGeneration).ToArray())
            {
                if (!Recoverable(diagnostic)) continue;
                var candidates = plan.Candidates.Where(d => Match(d, diagnostic) ||
                    d is FunctionDescriptor f && f.Fields.Any(p => Match(p, diagnostic))).ToArray();
                if (candidates.Length == 0) continue; // No proven owner: fail closed.
                // External inheritance cannot leave a managed class inheriting a
                // wrapper that has not been generated. Exclude the whole type.
                if (diagnostic.Message.StartsWith("Resolved external base class", StringComparison.Ordinal))
                    candidates = candidates.Select(d => d.Parent is ClassDescriptor cls ? cls : d).Distinct().ToArray();
                bool excluded = false;
                foreach (var candidate in candidates) excluded |= plan.Exclude(candidate);
                if (!excluded) continue;
                var entry = diagnostic;
                entry.Type = LogEntryType.InvalidHandleError;
                entry.Code = diagnostic.Code == "VETCPP001" ? "VETCPP005" : "VETCS005";
                entry.File ??= file.Path;
                entry.Message = diagnostic.Message + "\nSkipped binding: " +
                    string.Join(", ", candidates.Select(d => d.QualifiedName).Distinct()) +
                    ".";
                exclusions.Add(entry);
                // An export-plan error is otherwise reissued on every pass.
                plan.Errors.RemoveAll(d => d.Where.line == diagnostic.Where.line && d.Where.col == diagnostic.Where.col && d.Message == diagnostic.Message);
                progress = true;
            }
            return progress;
        }

        bool Match(DeclarationDescriptor declaration, LogEntry diagnostic) => declaration.Name.line == diagnostic.Where.line && declaration.Name.col == diagnostic.Where.col &&
            (diagnostic.File == null || declaration.DiagnosticFile?.Path == null || string.Equals(diagnostic.File, declaration.DiagnosticFile.Path, StringComparison.OrdinalIgnoreCase));
    }

    static bool Recoverable(LogEntry diagnostic)
    {
        string message = diagnostic.Message;
        if (diagnostic.Code == "VETCS001")
            return message.StartsWith("Cannot bind event", StringComparison.Ordinal) || message.StartsWith("No converter", StringComparison.Ordinal) ||
                message.StartsWith("Flax scripting object", StringComparison.Ordinal) ||
                message.StartsWith("Parameter '", StringComparison.Ordinal) && message.Contains("requires a shared ref converter", StringComparison.Ordinal) ||
                message.StartsWith("Concrete template", StringComparison.Ordinal) ||
                message.StartsWith("Resolved exported template", StringComparison.Ordinal) ||
                message.StartsWith("Resolved exported typedef", StringComparison.Ordinal) ||
                message.StartsWith("Exported typedef", StringComparison.Ordinal);
        if (diagnostic.Code == "VETCPP001")
            return message.StartsWith("Cannot bind native function", StringComparison.Ordinal) ||
                message.StartsWith("Flax scripting object", StringComparison.Ordinal) ||
                message.StartsWith("Cannot bind exported field", StringComparison.Ordinal) ||
                message.StartsWith("Cannot expose struct field", StringComparison.Ordinal) ||
                message.StartsWith("Reference return may alias", StringComparison.Ordinal) ||
                message.StartsWith("Native construction of managed structs", StringComparison.Ordinal) ||
                message.StartsWith("Native construction/ref-qualified receiver", StringComparison.Ordinal) ||
                message.StartsWith("Cannot access non-public", StringComparison.Ordinal) ||
                message.StartsWith("Native wrapper cannot access a non-public", StringComparison.Ordinal);
        if (diagnostic.Code == "VETCS003")
            return message.StartsWith("Unanchored native reference return", StringComparison.Ordinal) ||
                message.StartsWith("Struct receiver", StringComparison.Ordinal) ||
                message.StartsWith("Interface dispatch requires", StringComparison.Ordinal) ||
                message.StartsWith("Resolved external base class", StringComparison.Ordinal);
        return false;
    }
}
