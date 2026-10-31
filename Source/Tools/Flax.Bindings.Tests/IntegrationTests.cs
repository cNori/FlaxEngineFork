using Flax.Bindings;

string root = Path.Combine(Path.GetTempPath(), "FlaxBindingsV2-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(root);
int checks = 0;
void Check(bool value, string message) { if (!value) throw new Exception(message); checks++; }
string Header(string path, string source)
{
    path = Path.Combine(root, path); Directory.CreateDirectory(Path.GetDirectoryName(path)!);
    File.WriteAllText(path, source); return path;
}
string dependency = Header("Dependency.h", "API_CLASS(Static) class Dependency {public: API_FUNCTION() static int External();};");
string first = Header("First/Probe.h", """
    #include "Dependency.h"
    API_CLASS(Static) class First {public:
    #if BUILD_DEBUG
    API_FUNCTION() static int Debug(API_PARAM(DefaultValue=7) int value);
    #else
    API_FUNCTION() static int Release();
    #endif
    };
    """);
string second = Header("Second/Probe.h", "API_CLASS(Static) class Second {public: API_FUNCTION() static int Read(int first = 2, int last = Unknown());};");
ModuleRequest request = new()
{
    Headers = new[] { first, second }, IncludeDirectories = new[] { root },
    Definitions = new Dictionary<string, string> { ["BUILD_DEBUG"] = "1" },
    OutputDirectory = Path.Combine(root, "Output"), NativeLibrary = "Probe", CppStandard = 14
};
var generated = ModuleGenerator.Generate(request);
Check(generated.Success, string.Join("\n", generated.Diagnostics));
Check(generated.Files.Count == 4, "Only owned headers must emit; dependency is indexed only.");
Check(generated.Files.Select(f => f.Path).Distinct().Count() == 4, "Duplicate header basenames must not collide.");
string managed = string.Join("\n", generated.Files.Where(f => !f.Native).Select(f => f.Content));
Check(managed.Contains("Debug(int value = 7)") && !managed.Contains("Release("), "Actual build definitions must control parsing.");
Check(managed.Contains("Read(int last, int first = 2)"), "Parameter ordering/default metadata must survive integration.");
Check(managed.Contains("DllImport(\"Probe\""), "Native binary module name must reach P/Invoke.");
Check(!managed.Contains("class Dependency"), "Dependency ownership must not leak into output.");
ModuleGenerator.Publish(generated);
Check(generated.Files.All(f => File.ReadAllText(f.Path) == f.Content), "Publish must save exact paired output.");
var oldTime = new DateTime(2020, 1, 1, 0, 0, 0, DateTimeKind.Utc);
foreach (var file in generated.Files) File.SetLastWriteTimeUtc(file.Path, oldTime);
ModuleGenerator.Publish(ModuleGenerator.Generate(request));
Check(generated.Files.All(f => File.GetLastWriteTimeUtc(f.Path) == oldTime), "Unchanged generation must preserve incremental timestamps.");
var release = ModuleGenerator.Generate(request with { Definitions = new Dictionary<string, string>() });
string releaseText = string.Join("\n", release.Files.Where(f => !f.Native).Select(f => f.Content));
Check(release.Success && releaseText.Contains("Release()") && !releaseText.Contains("Debug("), "No hard-coded debug/editor profile may leak into build integration.");
Header("Second/Probe.h", "API_CLASS(Static) class Broken { public: API_FUNCTION() static void Read(API_PARAM(DefaultValue=) int value); };");
var broken = ModuleGenerator.Generate(request);
Check(!broken.Success, "Invalid metadata must fail the module.");
try { ModuleGenerator.Publish(broken); throw new Exception("Failed module publication was accepted."); }
catch (InvalidOperationException) { checks++; }
Check(generated.Files.All(f => File.ReadAllText(f.Path) == f.Content), "Failed module must preserve every previous output.");
Console.WriteLine($"Passed {checks} Flax.Build module adapter checks. Outputs: {root}");
