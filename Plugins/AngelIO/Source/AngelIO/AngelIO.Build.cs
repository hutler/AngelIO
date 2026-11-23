using UnrealBuildTool;
using System.IO;

public class AngelIO : ModuleRules
{
    public AngelIO(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
        PrivateDependencyModuleNames.Add("Json");
        string sdk = Path.Combine(ModuleDirectory, "..", "ThirdParty", "AngelScript");
        PublicSystemIncludePaths.Add(Path.Combine(sdk, "include"));
        PublicAdditionalLibraries.Add(Path.Combine(sdk, "lib", "Win64", "angelscript.lib"));
        if (Target.ProjectFile != null)
        {
            string root = Path.Combine(Target.ProjectFile.Directory.FullName, "Content", "AngelIO");
            if (Directory.Exists(root))
                foreach (string file in Directory.GetFiles(root, "*.as", SearchOption.AllDirectories))
                    RuntimeDependencies.Add(file, StagedFileType.UFS);
        }
    }
}
