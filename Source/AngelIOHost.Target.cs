using UnrealBuildTool;

public class AngelIOHostTarget : TargetRules
{
    public AngelIOHostTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("AngelIOHost");
    }
}
