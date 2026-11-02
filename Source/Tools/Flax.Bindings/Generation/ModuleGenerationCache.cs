using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Flax.Bindings;

/// <summary>Persistent results for unchanged modules; no mutable AST is shared between modules.</summary>
public static class ModuleGenerationCache
{
    public sealed record Output(string Path, string Hash, bool Native);
    public sealed record Dependency(string Path, string Hash);
    public sealed record Probe(string Path, bool Exists);
    public sealed record Entry(string Key, Output[] Outputs, Dependency[] Dependencies, Diagnostic[] Diagnostics, Probe[] Probes);
    sealed record DirectoryStamp(string Path, long Ticks);
    sealed record InventoryState(string[] Files, DirectoryStamp[] Directories);
    static readonly Dictionary<string, InventoryState> Inventories = new(StringComparer.OrdinalIgnoreCase);
    static readonly Dictionary<string, (long Length, long Ticks, string Hash)> Hashes = new(StringComparer.OrdinalIgnoreCase);
    static readonly object Gate = new();

    static string HashFile(string path)
    {
        var info = new FileInfo(path);
        lock (Gate)
        {
            if (Hashes.TryGetValue(path, out var known) && known.Length == info.Length && known.Ticks == info.LastWriteTimeUtc.Ticks) return known.Hash;
            using var stream = File.OpenRead(path);
            string hash = Convert.ToHexString(SHA256.HashData(stream));
            Hashes[path] = (info.Length, info.LastWriteTimeUtc.Ticks, hash);
            return hash;
        }
    }
    static bool Inside(string path, string root) => path.Equals(root, StringComparison.OrdinalIgnoreCase) || path.StartsWith(root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
    static string[] Inventory(string root)
    {
        lock (Gate)
        {
            if (Inventories.TryGetValue(root, out var inventory) && inventory.Directories.All(d => Directory.Exists(d.Path) && Directory.GetLastWriteTimeUtc(d.Path).Ticks == d.Ticks)) return inventory.Files;
            var files = Directory.Exists(root) ? Directory.EnumerateFiles(root, "*", new EnumerationOptions { RecurseSubdirectories = true, IgnoreInaccessible = false, AttributesToSkip = FileAttributes.ReparsePoint })
                .Where(p => Path.GetExtension(p).Equals(".h", StringComparison.OrdinalIgnoreCase)).Select(Path.GetFullPath).Order(StringComparer.OrdinalIgnoreCase).ToArray() : [];
            var directories = Directory.Exists(root) ? Directory.EnumerateDirectories(root, "*", new EnumerationOptions { RecurseSubdirectories = true, IgnoreInaccessible = false, AttributesToSkip = FileAttributes.ReparsePoint }).Prepend(root).Select(p => new DirectoryStamp(p, Directory.GetLastWriteTimeUtc(p).Ticks)).ToArray() : [];
            Inventories[root] = new(files, directories);
            return files;
        }
    }
    static string Key(ModuleRequest request)
    {
        var roots = request.ProjectDirectories.Concat(request.Headers.Select(h => Path.GetDirectoryName(Path.GetFullPath(h))!))
            .Select(Path.GetFullPath).Distinct(StringComparer.OrdinalIgnoreCase).OrderBy(p => p.Length).ToArray();
        var selected = new List<string>();
        foreach (string root in roots) if (!selected.Any(parent => Inside(root, parent))) selected.Add(root);
        var files = selected.SelectMany(Inventory).Concat(request.Headers.Select(Path.GetFullPath)).Distinct(StringComparer.OrdinalIgnoreCase).Order(StringComparer.OrdinalIgnoreCase);
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        void Add(string text) { hash.AppendData(Encoding.UTF8.GetBytes(text)); hash.AppendData([0]); }
        Add("module-cache-v2"); Add(typeof(ModuleGenerator).Assembly.ManifestModule.ModuleVersionId.ToString());
        Add(JsonSerializer.Serialize(request));
        var projects = request.ProjectDirectories.Select(Path.GetFullPath).ToArray();
        var headers = request.Headers.Select(Path.GetFullPath).ToHashSet(StringComparer.OrdinalIgnoreCase);
        // Included dependencies are validated separately. Unused SDK/external
        // headers need only membership tracking, not repeated content hashing.
        foreach (string file in files) { Add(file); if (headers.Contains(file) || projects.Any(root => Inside(file, root))) Add(HashFile(file)); }
        return Convert.ToHexString(hash.GetHashAndReset());
    }
    public static ModuleResult Generate(ModuleRequest request, string cachePath, out bool hit)
    {
        hit = false;
        string? key = null;
        try
        {
            key = Key(request);
            if (File.Exists(cachePath) && JsonSerializer.Deserialize<Entry>(File.ReadAllText(cachePath)) is { } cached && cached.Key == key && cached.Outputs != null && cached.Dependencies != null && cached.Diagnostics != null && cached.Probes != null && cached.Probes.All(p => File.Exists(p.Path) == p.Exists) &&
                cached.Dependencies.All(d => File.Exists(d.Path) && HashFile(d.Path) == d.Hash))
            {
                var outputs = new List<GeneratedFile>();
                foreach (var output in cached.Outputs)
                {
                    if (!File.Exists(output.Path)) { outputs.Clear(); break; }
                    string content = File.ReadAllText(output.Path);
                    if (Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(content))) != output.Hash) { outputs.Clear(); break; }
                    outputs.Add(new(output.Path, content, output.Native));
                }
                if (outputs.Count == cached.Outputs.Length)
                {
                    hit = true;
                    return new(outputs, cached.Diagnostics) { Dependencies = cached.Dependencies.Select(d => d.Path).ToArray(), IncludeResolutionProbes = cached.Probes.Select(p => p.Path).ToArray() };
                }
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { }
        var result = ModuleGenerator.Generate(request);
        // Missing includes could resolve to a previously nonexistent relative path
        // outside the indexed roots; keep those modules uncached until resolved.
        if (key != null && result.Success && !result.Diagnostics.Any(d => d.Text.Contains("VETP005", StringComparison.Ordinal)))
        {
            string? temporary = null;
            try
            {
                ModuleGenerator.Publish(result);
                var entry = new Entry(key, result.Files.Select(f => new Output(f.Path, Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(f.Content))), f.Native)).ToArray(),
                    result.Dependencies.Select(p => new Dependency(p, HashFile(p))).ToArray(), result.Diagnostics.ToArray(), result.IncludeResolutionProbes.Select(p => new Probe(p, File.Exists(p))).ToArray());
                Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(cachePath))!);
                temporary = cachePath + "." + Guid.NewGuid().ToString("N") + ".tmp";
                File.WriteAllText(temporary, JsonSerializer.Serialize(entry));
                File.Move(temporary, cachePath, true);
                temporary = null;
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
            finally { if (temporary != null && File.Exists(temporary)) File.Delete(temporary); }
        }
        return result;
    }
}
