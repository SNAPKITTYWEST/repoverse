using UnrealBuildTool;
public class Repoverse : ModuleRules
{
    public Repoverse(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] {"Core","CoreUObject","Engine","UnifyUnreal"});
    }
}
