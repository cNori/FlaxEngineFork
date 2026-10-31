// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Flax.Bindings;
using Flax.Build.NativeCpp;
using Flax.Build.Platforms;
using BuildData = Flax.Build.Builder.BuildData;

namespace Flax.Build.Bindings
{
    partial class BindingsGenerator
    {
        private static bool HasV2Exports(BuildOptions options) => options.SourceFiles
            .Where(p => p.EndsWith(".h", StringComparison.OrdinalIgnoreCase))
            .Any(p => System.IO.File.ReadAllText(p).Contains("Export", StringComparison.Ordinal));
        private static bool UseV2Generator
        {
            get
            {
                if (string.Equals(Configuration.BindingsGenerator, "V2", StringComparison.OrdinalIgnoreCase)) return true;
                if (string.Equals(Configuration.BindingsGenerator, "Legacy", StringComparison.OrdinalIgnoreCase)) return false;
                throw new BuildException("Unknown -bindingsGenerator value. Expected V2 or Legacy.");
            }
        }

        private static void GenerateV2Bindings(BuildData buildData, Module module, ModuleInfo moduleInfo,
            ref BuildOptions options, ref BindingsResult bindings)
        {
            if (buildData.Platform.Target != TargetPlatform.Windows || buildData.Architecture != TargetArchitecture.x64)
                throw new BuildException("V2 bindings currently support Windows x64 only. This target needs an ABI backend; generation was stopped.");
            var headers = options.SourceFiles.Where(p => p.EndsWith(".h", StringComparison.OrdinalIgnoreCase)).ToArray();
            if (options.SourceFiles.Any(p => p.EndsWith(".hpp", StringComparison.OrdinalIgnoreCase)))
                throw new BuildException("V2 bindings do not yet index .hpp files. Convert/index those headers before switching this module.");
            int standard = options.CompileEnv.CppVersion switch
            {
                CppVersion.Cpp14 => 14, CppVersion.Cpp17 => 17, CppVersion.Cpp20 => 20,
                _ => throw new BuildException("V2 requires an explicit C++ standard; CppVersion.Latest cannot be guessed.")
            };
            if (options.Toolchain is not WindowsToolchainBase windows || !Version.TryParse(windows.VCToolsVersion, out var compiler))
                throw new BuildException("V2 cannot determine the selected MSVC compiler's built-in definitions.");
            // Compiler built-ins are not present in /D lists. All build, editor,
            // platform and project definitions below come from resolved BuildOptions.
            var definitions = new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["_WIN32"] = "1", ["_WIN64"] = "1", ["_M_X64"] = "100", ["_CPPUNWIND"] = "1",
                ["_MSC_VER"] = ((compiler.Major + 5) * 100 + compiler.Minor).ToString(System.Globalization.CultureInfo.InvariantCulture),
                ["__cplusplus"] = "199711L",
                ["_MSVC_LANG"] = standard == 14 ? "201402L" : standard == 17 ? "201703L" : "202002L"
            };
            foreach (var definition in options.PublicDefinitions.Concat(options.PrivateDefinitions).Concat(options.CompileEnv.PreprocessorDefinitions))
            {
                var pair = definition.Split(new[] { '=' }, 2);
                definitions[pair[0]] = pair.Length == 2 ? pair[1] : "1";
            }
            var project = Builder.GetModuleProject(module, buildData);
            string output = Path.Combine(options.IntermediateFolder, "BindingsV2");
            Log.Verbose("Generating V2 API calls for " + module.Name);
            var generated = ModuleGenerator.Generate(new ModuleRequest
            {
                Headers = headers,
                IncludeDirectories = options.CompileEnv.IncludePaths.Concat(options.PublicIncludePaths).Concat(options.PrivateIncludePaths)
                    .Concat(new[] { Path.Combine(Globals.EngineRoot, "Source"), Path.Combine(project.ProjectFolderPath, "Source") })
                    .Where(Directory.Exists).Distinct(StringComparer.OrdinalIgnoreCase).ToArray(),
                ProjectDirectories = (string.Equals(Path.GetFullPath(project.ProjectFolderPath), Path.GetFullPath(Globals.EngineRoot), StringComparison.OrdinalIgnoreCase)
                    ? new[] { Path.Combine(Globals.EngineRoot, "Source", "Engine"), Path.Combine(Globals.EngineRoot, "Source", "Editor") }
                    : new[] { Path.Combine(Globals.EngineRoot, "Source", "Engine"), Path.Combine(project.ProjectFolderPath, "Source") })
                    .Where(Directory.Exists).Distinct(StringComparer.OrdinalIgnoreCase).ToArray(),
                Definitions = definitions, CppStandard = standard, OutputDirectory = output,
                NativeLibrary = module.BinaryModuleName,
                SharedManagedDirectory = Path.Combine(Path.GetDirectoryName(options.IntermediateFolder), "BindingsV2Shared", module.BinaryModuleName),
                ProjectClassLifetimes = true, RetainedLegacyNetworkRpc = true
            });
            foreach (var diagnostic in generated.Diagnostics)
            {
                if (diagnostic.Error) Log.Error(diagnostic.Text); else Log.Warning(diagnostic.Text);
            }
            if (!generated.Success)
                throw new BuildException("V2 bindings failed for module " + module.Name + "; compilation was not scheduled for that module.");
            foreach (var warning in ModuleGenerator.Publish(generated)) Log.Warning(warning);
            // Project generation must consume the same active files as the compiler,
            // rather than globbing stale content-addressed snapshots from the cache.
            Utilities.WriteFileIfChanged(Path.Combine(options.IntermediateFolder, module.Name + ".BindingsV2.ManagedFiles"),
                string.Join("\n", generated.Files.Where(f => !f.Native).Select(f => f.Path).Distinct(StringComparer.OrdinalIgnoreCase)) + "\n");
            bindings.AdditionalCppFiles = generated.Files.Where(f => f.Native).Select(f => f.Path).ToList();
            foreach (var file in generated.Files.Where(f => !f.Native))
                if (!options.SourceFiles.Contains(file.Path)) options.SourceFiles.Add(file.Path);
            // Replace the old aggregate managed file so a previous legacy result
            // can never be picked up alongside the new per-header sources.
            Utilities.WriteFileIfChanged(bindings.GeneratedCSharpFilePath, "// V2 managed bindings are compiled from the BindingsV2 directory.\n");
            if (!options.SourceFiles.Contains(bindings.GeneratedCSharpFilePath)) options.SourceFiles.Add(bindings.GeneratedCSharpFilePath);
            // Legacy parse-cache hits must not bypass V2 generation or membership
            // registration. Regenerate from current headers on each build setup;
            // Publish preserves timestamps for unchanged content.
            // Retained registration/wrappers must also reflect generator changes.
            // Write-if-changed publication preserves native compilation timestamps.
            moduleInfo.Init(buildData);
            GenerateCpp(buildData, moduleInfo, ref bindings);
            GenerateBinaryModuleBindings?.Invoke(buildData, moduleInfo, ref bindings);
        }
    }
}
