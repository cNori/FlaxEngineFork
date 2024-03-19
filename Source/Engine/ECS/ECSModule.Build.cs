// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

using Flax.Build;
using Flax.Build.NativeCpp;

/// <summary>
/// User Interface module.
/// </summary>
public class ECS : EngineModule
{
    /// <inheritdoc />
    public override void Setup(BuildOptions options)
    {
        base.Setup(options);
        options.CompileEnv.CppVersion = CppVersion.Cpp17;
    }
}
