using System.Security.Cryptography;
using System.Text;
using VETools;
using VETools.Generation;
using VETools.Text;

namespace Flax.Bindings;

/// <summary>The resolved module environment supplied by Flax.Build.</summary>
public sealed record ModuleRequest
{
    public required IReadOnlyList<string> Headers { get; init; }
    public required IReadOnlyList<string> IncludeDirectories { get; init; }
    public IReadOnlyList<string> ProjectDirectories { get; init; } = [];
    public required IReadOnlyDictionary<string, string> Definitions { get; init; }
    public required string OutputDirectory { get; init; }
    public required string NativeLibrary { get; init; }
    public int CppStandard { get; init; } = 14;
    /// <summary>The build uses V2 lifetime wrappers for referenced project classes too.</summary>
    public bool ProjectClassLifetimes { get; init; }
    /// <summary>The host retains legacy native RPC registration for Hidden methods.</summary>
    public bool RetainedLegacyNetworkRpc { get; init; }
    /// <summary>Common directory for converter value declarations within one managed assembly.</summary>
    public string? SharedManagedDirectory { get; init; }
}

public sealed record Diagnostic(string Text, bool Error, bool Blocking);
public sealed record GeneratedFile(string Path, string Content, bool Native);
public sealed record ModuleResult(IReadOnlyList<GeneratedFile> Files, IReadOnlyList<Diagnostic> Diagnostics)
{
    public bool Success => !Diagnostics.Any(d => d.Blocking);
}

/// <summary>In-process API; no console host, pauses, or implicit build profile.</summary>
public static class ModuleGenerator
{
    public static ModuleResult Generate(ModuleRequest request)
    {
        var headers = request.Headers.Select(Path.GetFullPath).Distinct(StringComparer.OrdinalIgnoreCase)
            .Order(StringComparer.OrdinalIgnoreCase).ToArray();
        if (headers.Length == 0) return new([], []);
        if (headers.Any(h => !Path.GetExtension(h).Equals(".h", StringComparison.OrdinalIgnoreCase)))
            return new([], [new("V2 bindings currently accept .h headers only.", true, true)]);
        List<Diagnostic> diagnostics = [];
        List<GeneratedFile> outputs = [];
        var seen = new HashSet<string>(StringComparer.Ordinal);
        void Report(LogEntry entry, string path)
        {
            string text = entry.Format(path);
            if (seen.Add(text)) diagnostics.Add(new(text, entry.IsError, entry.BlocksGeneration));
        }
        var project = SourceProject.Load(headers[0], request.IncludeDirectories, headers.Skip(1), request.CppStandard,
            path => { var registry = ConverterConfiguration.Create(path); registry.RetainedLegacyNetworkRpc = request.RetainedLegacyNetworkRpc; return registry; }, request.Definitions, request.ProjectDirectories);
        project.Analyze();
        if (request.ProjectClassLifetimes)
        {
            // This is an explicit build-wide ABI agreement, not an inference
            // from finding a base class declaration in an arbitrary include.
            var contracts = project.Files.SelectMany(TemplateInstantiationPass.Declarations).OfType<ClassDescriptor>()
                .Where(c => !c.Api.InBuild && !c.Api.Static && !c.Api.Template && !c.IsForwardDeclaration &&
                    c.MarshalAsType == null && c.Api.MarshalAsTokens.Count == 0 &&
                    !HandleNETcpp.IsScriptingObject(c) && ExportAttributes.Read(c).Export &&
                    !(c.SourceFile?.Converters.TryGet(c.QualifiedName, out var converter) == true && converter.ExistingManagedType) &&
                    !c.Api.Errors.Any(e => e.BlocksGeneration) && c.SourceFile?.Errors.Any(e => e.BlocksGeneration) != true)
                .ToHashSet();
            bool EligibleBases(ClassDescriptor type)
            {
                for (DeclarationDescriptor current = type; current.Parent is TypeDescriptor owner; current = owner)
                    if (current.Access != "public" || !ExportAttributes.Read(owner).Export) return false;
                int classes = 0;
                foreach (var basis in type.BaseTypes)
                {
                    if (basis.Access != "public" || basis.Type is not NamedTypeInfo { Symbol.Definition: TypeDescriptor definition }) return false;
                    if (definition is ClassDescriptor parent) { if (++classes > 1 || !contracts.Contains(parent)) return false; }
                    else if (definition is not InterfaceDescriptor || !ExportAttributes.Read(definition).Export) return false;
                }
                return true;
            }
            while (contracts.RemoveWhere(c => !EligibleBases(c)) != 0) { }
            foreach (var file in project.Files) file.ProjectClassLifetimes = contracts;
        }
        foreach (var entry in project.Diagnostics) Report(entry, headers[0]);
        var sharedDeclarations = new HashSet<string>(StringComparer.Ordinal);
        var owned = headers.ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (var file in project.Files.Where(f => f.Path != null && owned.Contains(f.Path)))
        {
            foreach (var entry in file.Errors) Report(entry, file.Path!);
            if (!ExportPlan.Build(file).Candidates.Any()) continue;
            var generated = BindingGeneration.Generate(file, "windows-x64", file.Path,
                library: request.NativeLibrary, separateSharedDeclarations: true);
            foreach (var entry in generated.Managed.Notes.Concat(generated.Managed.Diagnostics)
                .Concat(generated.Native?.Diagnostics ?? []).Concat(generated.Interop?.Diagnostics ?? []))
                Report(entry, file.Path!);
            if (!generated.Managed.Success || generated.Native?.Success != true || generated.Interop?.Success != true)
                continue;
            // Flat filenames are collision-free even when two modules contain Foo.h.
            string id = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(file.Path!)))[..16];
            string stem = Path.Combine(Path.GetFullPath(request.OutputDirectory), Path.GetFileNameWithoutExtension(file.Path!) + "." + id);
            outputs.Add(new(stem + ".g.cpp", generated.Native.Source, true));
            outputs.Add(new(stem + ".g.cs", generated.Interop.Source, false));
            sharedDeclarations.UnionWith(generated.Managed.SharedManagedDeclarations);
        }
        foreach (string declaration in sharedDeclarations.Order(StringComparer.Ordinal))
        {
            // Content-addressed paths are shared across modules in this assembly.
            // Per-header converter helper partials remain local and uniquely named.
            string id = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(declaration)));
            string path = Path.Combine(Path.GetFullPath(request.SharedManagedDirectory ?? request.OutputDirectory), "ValueSnapshot." + id + ".g.cs");
            outputs.Add(new(path, "// <auto-generated />\n#nullable disable\nusing global::System;\nusing global::System.ComponentModel;\nusing global::System.Runtime.InteropServices;\n" + declaration, false));
        }
        return new(outputs, diagnostics);
    }

    /// <summary>Publish only a successful module; unchanged files keep their timestamps.</summary>
    public static IReadOnlyList<string> Publish(ModuleResult result)
    {
        if (!result.Success) throw new InvalidOperationException("Cannot publish failed V2 module generation.");
        var changes = result.Files.Where(f => !File.Exists(f.Path) || File.ReadAllText(f.Path) != f.Content)
            .Select(f => new OutputPublisher.Output(f.Path, f.Content)).ToArray();
        return changes.Length != 0 ? OutputPublisher.Write(changes) : [];
    }
}
