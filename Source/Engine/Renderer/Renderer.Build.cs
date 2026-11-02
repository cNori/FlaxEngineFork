// Copyright (c) Wojciech Figat. All rights reserved.

using Flax.Build;
using Flax.Build.NativeCpp;

/// <summary>
/// 3D graphics rendering module.
/// </summary>
public class Renderer : EngineModule
{
    /// <inheritdoc />
    public override void SetupEnvironment(BuildOptions options)
    {
        base.SetupEnvironment(options);
        // Private/Renderer.cpp duplicates the module's root implementation.
        // Compile the root once; both files also collide on Renderer.obj.
        var duplicate = System.IO.Path.GetFullPath(System.IO.Path.Combine(FolderPath, "Private", "Renderer.cpp"));
        options.SourceFiles.RemoveAll(path => string.Equals(System.IO.Path.GetFullPath(path), duplicate, System.StringComparison.OrdinalIgnoreCase));
    }

    /// <inheritdoc />
    public override void Setup(BuildOptions options)
    {
        base.Setup(options);

        options.PrivateDependencies.Add("Graphics");
        options.PrivateDependencies.Add("Content");
        if (options.Target.IsEditor)
        {
            options.PrivateDependencies.Add("TextureTool");
        }
    }
}
