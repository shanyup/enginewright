using UnrealBuildTool;

public class Enginewright : ModuleRules
{
	public Enginewright(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "Json", "JsonUtilities", "HTTPServer"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd", "Projects", "Slate", "SlateCore", "AssetRegistry", "AssetTools", "DeveloperSettings"
		});
	}
}
