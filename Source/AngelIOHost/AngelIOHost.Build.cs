using UnrealBuildTool;

public class AngelIOHost : ModuleRules
{
    public AngelIOHost(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "AngelIO" });
        PrivateDependencyModuleNames.AddRange(new[] { "Json", "InputCore" });
        if (Target.bBuildEditor)
            PrivateDependencyModuleNames.AddRange(new[] { "UnrealEd", "AngelIOEditor", "Slate", "SlateCore" });
    }
}
