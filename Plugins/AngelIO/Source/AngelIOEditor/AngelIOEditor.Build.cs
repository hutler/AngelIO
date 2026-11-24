using UnrealBuildTool;

public class AngelIOEditor : ModuleRules
{
    public AngelIOEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "SlateCore", "AngelIO" });
        PrivateDependencyModuleNames.AddRange(new[] { "Engine", "EngineSettings", "Json", "UnrealEd", "Slate", "Sockets", "ToolMenus", "WorkspaceMenuStructure" });
    }
}
