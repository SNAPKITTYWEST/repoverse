using UnrealBuildTool;
public class RepoverseTarget : TargetRules
{
    public RepoverseTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.V5;
        ExtraModuleNames.Add("Repoverse");
    }
}
