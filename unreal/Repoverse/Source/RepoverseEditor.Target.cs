using UnrealBuildTool;
public class RepoverseEditorTarget : TargetRules
{
    public RepoverseEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.V5;
        ExtraModuleNames.Add("Repoverse");
    }
}
