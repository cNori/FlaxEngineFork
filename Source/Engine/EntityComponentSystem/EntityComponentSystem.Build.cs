using System.Collections.Generic;
using System.IO;
using Flax.Build;
using Flax.Build.NativeCpp;

namespace BuildScripts
{
    /// <summary>
    /// Content module.
    /// </summary>
    public class EntityComponentSystem : EngineModule
    {
        public override void Setup(BuildOptions options)
        {
            base.Setup(options);
        }
        public override void GetFilesToDeploy(List<string> files)
        {
            files.AddRange(Directory.GetFiles(FolderPath, "*.h", SearchOption.AllDirectories));
        }
    }
}
