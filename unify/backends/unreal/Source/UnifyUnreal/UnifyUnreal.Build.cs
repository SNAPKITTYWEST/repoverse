// Links the platform-independent UNIFY engine (built with CMake: libunify_engine.a + liblua.a)
// into an Unreal runtime module. Set UNIFY_ROOT to the UNIFY checkout and UNIFY_BUILD to its
// CMake build directory before generating project files.
using System;
using System.IO;
using UnrealBuildTool;

public class UnifyUnreal : ModuleRules
{
    public UnifyUnreal(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "InputCore", "AudioMixer", "SlateCore" });

        string Root = Environment.GetEnvironmentVariable("UNIFY_ROOT") ?? Path.Combine(ModuleDirectory, "../../../..");
        string Build = Environment.GetEnvironmentVariable("UNIFY_BUILD") ?? Path.Combine(Root, "build");
        PublicIncludePaths.Add(Path.Combine(Root, "engine"));
        PublicIncludePaths.Add(Path.Combine(Root, "third_party/lua/src"));

        string Lib = Target.Platform == UnrealTargetPlatform.Win64 ? ".lib" : ".a";
        string Prefix = Target.Platform == UnrealTargetPlatform.Win64 ? "" : "lib";
        PublicAdditionalLibraries.Add(Path.Combine(Build, Prefix + "unify_engine" + Lib));
        PublicAdditionalLibraries.Add(Path.Combine(Build, Prefix + "lua" + Lib));

        // UNIFY replaces global operator new for allocation accounting; Unreal owns operator new
        // in its own modules, so the engine library must be built with UNIFY_NO_GLOBAL_NEW=ON for UE.
        PublicDefinitions.Add("UNIFY_UNREAL=1");
        bEnableExceptions = true;  // Lua is compiled as C++ and reports script errors via exceptions
    }
}
