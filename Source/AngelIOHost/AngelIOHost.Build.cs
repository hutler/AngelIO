using UnrealBuildTool;

public class AngelIOHost : ModuleRules
{
    public AngelIOHost(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "AngelIO" });
        PrivateDependencyModuleNames.Add("Json");
        if (Target.bBuildEditor)
            PrivateDependencyModuleNames.Add("UnrealEd");
    }
}
