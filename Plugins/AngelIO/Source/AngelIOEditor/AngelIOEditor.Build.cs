using UnrealBuildTool;

public class AngelIOEditor : ModuleRules
{
    public AngelIOEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "AngelIO", "Json", "UnrealEd" });
    }
}
