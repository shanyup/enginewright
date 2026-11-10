using UnrealBuildTool;

public class Enginewright : ModuleRules
{
	public Enginewright(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Each tool file keeps small helpers (Bad, Registry, ...) in an anonymous namespace; unity builds would merge them.
		bUseUnity = false;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "Json", "JsonUtilities", "HTTPServer"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd", "Projects", "Slate", "SlateCore", "AssetRegistry", "AssetTools", "DeveloperSettings",
			"BlueprintGraph", "Kismet", "KismetCompiler", "GraphEditor", "ImageWrapper", "InputCore", "EnhancedInput",
			"MaterialEditor", "LevelEditor", "EditorSubsystem", "DataValidation", "RenderCore", "RHI", "PhysicsCore",
			"UMG", "UMGEditor", "GameplayTags", "GameplayTagsEditor", "NavigationSystem",
			"ToolMenus", "WorkspaceMenuStructure", "Settings", "ApplicationCore", "MeshDescription", "StaticMeshDescription"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}
	}
}
