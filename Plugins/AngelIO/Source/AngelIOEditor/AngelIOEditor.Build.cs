using UnrealBuildTool;

public class AngelIOEditor : ModuleRules
{
    public AngelIOEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "SlateCore", "AngelIO" });
        PrivateDependencyModuleNames.AddRange(new[] { "Engine", "Json", "UnrealEd", "Slate", "Sockets", "WorkspaceMenuStructure" });
    }
}
