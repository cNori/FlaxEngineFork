
using Flax.Build;
using Flax.Build.NativeCpp;
using System.IO;
using System.Collections.Generic;

/// <summary>
/// User Interface module.
/// </summary>
public class ECS : EngineModule
{
    /// <inheritdoc />
    public override void Setup(BuildOptions options)
    {
        base.Setup(options);
        options.CompileEnv.PrecompiledHeaderUsage = PrecompiledHeaderFileUsage.None;
        options.LinkEnv.Output = LinkerOutput.SharedLibrary;
        options.CompileEnv.CppVersion = CppVersion.Cpp17;
    }
}
