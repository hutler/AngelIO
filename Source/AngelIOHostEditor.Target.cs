using UnrealBuildTool;

public class AngelIOHostEditorTarget : TargetRules
{
    public AngelIOHostEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("AngelIOHost");
    }
}
