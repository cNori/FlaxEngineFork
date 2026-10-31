using System.Text.RegularExpressions;

namespace VETools.Text;

internal sealed class SourceProject
{
    public required FileDescriptor Root { get; init; }
    public List<FileDescriptor> Files { get; } = [];
    public List<LogEntry> Diagnostics { get; } = [];
    public int DiscoveredHeaderCount { get; private set; }

    public static SourceProject Load(string rootPath, IEnumerable<string>? includeDirectories = null,
        IEnumerable<string>? references = null, int cppStandard = 20,
        Func<string, TypeConverterRegistry>? converters = null,
        IReadOnlyDictionary<string, string>? definitions = null,
        IEnumerable<string>? projectDirectories = null, IEnumerable<string>? excludedDirectories = null)
    {
        var roots = (includeDirectories ?? []).Select(Path.GetFullPath).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        var files = new Dictionary<string, FileDescriptor>(StringComparer.OrdinalIgnoreCase);
        var macros = (definitions ?? new Dictionary<string, string>()).ToDictionary(p => p.Key, p => new PreprocessorMacro(p.Value), StringComparer.Ordinal);
        var profile = new Dictionary<string, PreprocessorMacro>(macros, StringComparer.Ordinal);
        var effects = new Dictionary<string, List<(string Name, PreprocessorMacro? Value, bool Include)>>(StringComparer.OrdinalIgnoreCase);
        var applied = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        Stack<List<(string Name, PreprocessorMacro? Value, bool Include)>> effectScopes = new();
        List<Action<string, PreprocessorMacro?>> macroObservers = [];
        var excluded = (excludedDirectories ?? []).Select(Path.GetFullPath).ToArray();
        List<LogEntry> logs = [];
        static bool Header(string path) => string.Equals(Path.GetExtension(path), ".h", StringComparison.OrdinalIgnoreCase);
        FileDescriptor Read(string input)
        {
            string path = Path.GetFullPath(input);
            if (!Header(path)) throw new ArgumentException("Only .h source headers may be indexed: " + path);
            if (files.TryGetValue(path, out var existing))
            {
                // Parsing is cached, but includes still supply definitions to the
                // current standalone header's preprocessor context. An in-progress
                // include cycle has no completed effects to replay.
                if (applied.Add(path) && effects.TryGetValue(path, out var changes))
                    foreach (var (name, value, include) in changes)
                    {
                        if (include) Read(name);
                        else
                        {
                            if (value == null) macros.Remove(name); else macros[name] = value;
                            foreach (var observer in macroObservers) observer(name, value);
                        }
                    }
                return existing;
            }
            applied.Add(path);
            List<(string Name, PreprocessorMacro? Value, bool Include)> ownEffects = [];
            effectScopes.Push(ownEffects);
            string source = System.IO.File.ReadAllText(path);
            FileDescriptor file = new() { Path = path, OriginalSourceText = source, IncludeNamespace = IncludeNamespace(path) };
            files.Add(path, file);
            var initialMacros = new Dictionary<string, PreprocessorMacro>(macros, StringComparer.Ordinal);
            List<(int Line, string Name, PreprocessorMacro? Value)> localChanges = [];
            List<LogEntry> preprocessorLogs = [];
            string active = HeaderPreprocessor.Filter(source, path, macros, (body, where) =>
            {
                // External-style includes are outside the declaration index.
                if (body.StartsWith('<')) return;
                bool macroInclude = !body.StartsWith('"');
                string headerText = body;
                if (macroInclude)
                {
                    var expanded = DefaultMacroExpansion.Expand(Tokenizer.Tokenize(body), macros);
                    if (expanded == null) return;
                    headerText = string.Concat(expanded.Select(t => t.text));
                    // Undefined aliases (for example FT_FREETYPE_H supplied by an
                    // ignored external header) and expanded angle headers are skipped.
                    if (!headerText.StartsWith('"')) return;
                }
                var literal = Regex.Match(headerText, "^\"(?<quoted>[^\"\\r\\n]+)\"(?<tail>.*)$", RegexOptions.Singleline);
                if (!literal.Success || Tokenizer.Tokenize(literal.Groups["tail"].Value).Any(t => !t.IsTrivia && t.kind != Tokenizer.Token.Kind.EndOfFile))
                {
                    preprocessorLogs.Add(new($"Cannot expand include '{body}' to one quoted project header name. Index the .h file with --reference if needed.", where, LogEntryType.Warning, "VETP005", path)); return;
                }
                string include = literal.Groups["quoted"].Value;
                if (!Header(include)) return;
                string? resolved = new[] { Path.GetDirectoryName(path)! }.Concat(roots)
                    .Select(directory => Path.GetFullPath(Path.Combine(directory, include))).FirstOrDefault(System.IO.File.Exists);
                if (resolved == null)
                {
                    preprocessorLogs.Add(new($"Cannot resolve active include '{include}'. Supply an --include-dir search path; its types are not indexed.", where, LogEntryType.Warning, "VETP005", path)); return;
                }
                ownEffects.Add((resolved, null, true));
                Action<string, PreprocessorMacro?> observer = (name, value) => localChanges.Add((where.line, name, value));
                macroObservers.Add(observer);
                try { Read(resolved); }
                finally { macroObservers.Remove(observer); }
            }, preprocessorLogs, Record, (name, value, where) => localChanges.Add((where.line, name, value)));
            effectScopes.Pop();
            var parsed = Parshe.PreParse(new Tokenizer.Stream(Tokenizer.Tokenize(active)), [], cppStandard, converters?.Invoke(path), macros.Where(m => m.Value.FunctionLike).Select(m => m.Key));
            file.CppStandard = parsed.CppStandard; file.Converters = parsed.Converters; file.MacroDefinitions = parsed.MacroDefinitions;
            file.Tokens = parsed.Tokens; file.Errors = parsed.Errors; file.SourceText = active;
            foreach (var declaration in parsed.Declarations) file.Add(declaration);
            DefaultMacroExpansion.Apply(file, initialMacros, localChanges);
            file.Errors.AddRange(preprocessorLogs.Where(entry => entry.IsError));
            logs.AddRange(preprocessorLogs);
            effects[path] = ownEffects;
            return file;
        }
        FileDescriptor ReadStandalone(string path)
        {
            macros.Clear();
            applied.Clear();
            foreach (var (name, value) in profile) macros.Add(name, value);
            return Read(path);
        }
        var root = ReadStandalone(rootPath);
        foreach (var reference in references ?? []) ReadStandalone(reference);
        var discovered = new SortedSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var directory in (projectDirectories ?? []).Select(Path.GetFullPath).Distinct(StringComparer.OrdinalIgnoreCase))
            Discover(directory);
        foreach (var path in discovered) if (!files.ContainsKey(path)) ReadStandalone(path);
        SourceProject project = new() { Root = root };
        project.DiscoveredHeaderCount = discovered.Count;
        project.Files.AddRange(files.Values); project.Diagnostics.AddRange(logs);
        return project;

        void Discover(string directory)
        {
            if (excluded.Any(path => Within(directory, path))) return;
            foreach (var path in Directory.EnumerateFiles(directory).Where(Header)) discovered.Add(Path.GetFullPath(path));
            foreach (var child in Directory.EnumerateDirectories(directory).Order(StringComparer.OrdinalIgnoreCase))
            {
                // Generated/build output and links are not independent project sources.
                if (Path.GetFileName(child) is ".git" or ".vs" or "bin" or "obj" or "Artifacts") continue;
                if ((System.IO.File.GetAttributes(child) & FileAttributes.ReparsePoint) != 0) continue;
                Discover(child);
            }
        }
        static bool Within(string path, string parent) => string.Equals(path.TrimEnd(Path.DirectorySeparatorChar), parent.TrimEnd(Path.DirectorySeparatorChar), StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith(parent.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
        string IncludeNamespace(string path)
        {
            string directory = Path.GetDirectoryName(path)!;
            string? root = roots.FirstOrDefault(r => Within(path, r));
            string relative = root == null ? Path.GetFileName(directory) : Path.GetRelativePath(root, directory);
            if (relative == ".") return "";
            return string.Join(".", relative.Split([Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar], StringSplitOptions.RemoveEmptyEntries)
                .Select(part => { string name = Regex.Replace(part, @"[^A-Za-z0-9_]", "_"); return char.IsDigit(name[0]) ? "_" + name : name; }));
        }
        void Record(string name, PreprocessorMacro? value)
        {
            effectScopes.Peek().Add((name, value, false));
            foreach (var observer in macroObservers) observer(name, value);
        }
    }

    public void Analyze() => TypeInfoPass.RunProject(Files);
}
