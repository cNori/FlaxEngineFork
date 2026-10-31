namespace VETools.Text;

// Project-specific converter registrations live here as ordinary C# code.
// Rebuilt with the tool; no JSON, runtime script engine or alias discovery.
internal static class ConverterConfiguration
{
    // Explicit build profile; command-line --define/--undefine can override it.
    public static Dictionary<string, string> CreateDefines(int cppStandard) => new(StringComparer.Ordinal)
    {
        ["PLATFORM_WINDOWS"] = "1", ["PLATFORM_WIN32"] = "1", ["WIN32"] = "1", ["WIN64"] = "1",
        ["_WIN32"] = "1", ["_WIN64"] = "1", ["_M_X64"] = "100", ["PLATFORM_64BITS"] = "1",
        ["_MSC_VER"] = "1940", ["_CPPUNWIND"] = "1", ["__cplusplus"] = "199711L",
        ["_MSVC_LANG"] = cppStandard switch { 11 => "201103L", 14 => "201402L", 17 => "201703L", 20 => "202002L", _ => "202302L" },
        ["BUILD_DEBUG"] = "1", ["USE_EDITOR"] = "1", ["COMPILE_WITH_CSHARP"] = "1"
    };
    public static TypeConverterRegistry Create(string inputPath) => new(registry => Configure(registry, inputPath));

    static void Configure(TypeConverterRegistry registry, string inputPath)
    {
        // This tool currently targets the Windows x64 Flax engine build.
        FlaxTypeConverters.RegisterWindowsX64Primitives(registry);
        string path = inputPath.Replace('\\', '/');
        bool engineSource = path.Contains("/Source/Engine/", StringComparison.OrdinalIgnoreCase), editorSource = path.Contains("/Source/Editor/", StringComparison.OrdinalIgnoreCase);
        if (engineSource || editorSource)
        {
            registry.DefaultManagedNamespace = editorSource ? "FlaxEditor" : "FlaxEngine";
            registry.CanonicalValueRecords = true;
            registry.ManagedUsings.Add("FlaxEngine");
            foreach (string name in new[] { "Float2", "Float3", "Float4", "Double2", "Double3", "Double4", "Int2", "Int3", "Int4", "Quaternion", "Transform", "Color", "BoundingBox", "BoundingSphere", "Rectangle", "Ray", "Plane" })
                registry.RegisterManagedValuePartial(name, "global::FlaxEngine." + name);
            registry.RegisterManagedValuePartial("Matrix", "global::FlaxEngine.Matrix", generateFields: false);
            registry.RegisterManagedValuePartial("Matrix3x3", "global::FlaxEngine.Matrix3x3", generateFields: false);
        }
        registry.FlaxScriptingObjects = true;
        registry.RegisterManagedGenericType("JsonAssetReference", "global::FlaxEngine.JsonAssetReference");
        registry.RegisterOpaqueHandleProperty("WindowBase::CursorImage");
        registry.RegisterInputPointerArray("DebugDraw::DrawActors", "selectedActors", "selectedActorsCount");
        registry.FlaxDelegateLifetimes = true;
        foreach (string name in new[] { "CollectDrawCalls", "PreRender", "PostRender" }) registry.RegisterSynchronousEvent("SceneRenderTask::" + name);
        // Connection validation must deny the connection if a subscribed managed
        // handler cannot run or cannot return a successfully converted result.
        registry.RegisterSynchronousEvent("NetworkManager::ClientConnecting", "arg0.Result = 1;");
        registry.RegisterSynchronousEvent("Engine::ReportCrash", "::Platform::Error(arg0);", new() { Includes = ["\"Engine/Platform/Platform.h\""] });
        registry.RegisterBorrowedEventPointer("Engine::ReportCrash", 1);
        registry.FlaxContainers = true;
        registry.RegisterContainerAllocator("RendererAllocation");
        registry.FlaxFunctions = true;
        FlaxStringConverters.RegisterWindowsX64(registry);
        FlaxGuidConverter.Register(registry);
        FlaxVariantConverter.Register(registry);
        FlaxAbiConverters.Register(registry);
        FlaxHalfConverter.Register(registry);
        registry.RegisterExportedSettingsSnapshot("ModelTool::Options");
        registry.RegisterDeclarationMacro("DEPRECATED", DeclarationMacroKind.Deprecated);
        foreach(string macro in new[]{"DECLARE_SCRIPTING_TYPE_MINIMAL","DECLARE_SCRIPTING_TYPE_STRUCTURE","DECLARE_SCRIPTING_TYPE_NO_SPAWN","DECLARE_SCRIPTING_TYPE","DECLARE_SCRIPTING_TYPE_WITH_CONSTRUCTOR_IMPL",
            "DECLARE_SCENE_OBJECT","DECLARE_SCENE_OBJECT_ABSTRACT","DECLARE_SCENE_OBJECT_NO_SPAWN","DECLARE_ASSET_HEADER","DECLARE_BINARY_ASSET_HEADER"})
            registry.RegisterDeclarationMacro(macro,DeclarationMacroKind.PublicDeclarations, preservesInstanceLayout: true);
        // Use inputPath to choose a project-specific map when needed.
        // registry.RegisterAlias("Game::Count", "int");
        // registry.RegisterConverter("Game::Id", "unsigned long long",
        //     toAbi: value => $"({value}).Value",
        //     toNative: value => $"::Game::Id::FromRaw({value})");
    }
}


