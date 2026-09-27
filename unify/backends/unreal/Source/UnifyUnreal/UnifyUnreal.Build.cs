using System;
using System.IO;
using UnrealBuildTool;
public class UnifyUnreal : ModuleRules
{
    public UnifyUnreal(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp17;
        bEnableExceptions = true;
        bUseRTTI = true;
        PublicDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "InputCore", "AudioMixer", "SlateCore", "RenderCore", "RHI"
        });
        string Root = Environment.GetEnvironmentVariable("UNIFY_ROOT");
        string Build = Environment.GetEnvironmentVariable("UNIFY_BUILD");
        if (String.IsNullOrEmpty(Root) || String.IsNullOrEmpty(Build))
            throw new BuildException("Run unreal/Prepare-Unreal.ps1 in this shell to set UNIFY_ROOT and UNIFY_BUILD.");
        PublicIncludePaths.Add(Path.Combine(Root, "engine"));
        PublicIncludePaths.Add(Path.Combine(Root, "third_party/lua/src"));
        string Prefix = Target.Platform == UnrealTargetPlatform.Win64 ? "" : "lib";
        string Ext = Target.Platform == UnrealTargetPlatform.Win64 ? ".lib" : ".a";
        foreach (string Name in new[] {"unify_engine", "lua"})
        {
            string Library = Path.Combine(Build, Prefix + Name + Ext);
            if (!File.Exists(Library))
                throw new BuildException("Missing UNIFY library: " + Library);
            PublicAdditionalLibraries.Add(Library);
        }
        PublicDefinitions.Add("UNIFY_UNREAL=1");
        PublicDefinitions.Add("UNIFY_NO_GLOBAL_NEW=1");
        string Assets = Path.Combine(Target.ProjectFile.Directory.FullName, "Unify", "assets");
        if (Directory.Exists(Assets))
            foreach (string FilePath in Directory.GetFiles(Assets, "*", SearchOption.AllDirectories))
                if (!FilePath.EndsWith(".usav"))
                    RuntimeDependencies.Add(FilePath, StagedFileType.NonUFS);
    }
}
