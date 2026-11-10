using UnrealBuildTool;

// Holds every dependency on Epic's MVVM plugin, so the main Enginewright module needs none of it.
public class EnginewrightMVVM : ModuleRules
{
	public EnginewrightMVVM(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "Json", "UnrealEd", "UMG", "UMGEditor", "BlueprintGraph", "Kismet", "KismetCompiler",
			"Enginewright",
			"ModelViewViewModel", "ModelViewViewModelBlueprint", "ModelViewViewModelEditor", "FieldNotification", "PropertyPath"
		});
	}
}
