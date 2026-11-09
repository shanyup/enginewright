#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "EnginewrightSettings.generated.h"

/** How much Enginewright writes to the Output Log. */
UENUM()
enum class EEnginewrightLogLevel : uint8
{
	/** Start-up line and errors only. */
	Quiet,
	/** One line per tool call: name, result, time. */
	Calls,
	/** One line per tool call with its arguments. */
	Verbose,
};

/**
 * Enginewright settings, per user and per project (Saved/Config/<Platform>Editor/EditorPerProjectUserSettings.ini).
 * Shown in Project Settings > Plugins > Enginewright and in the Enginewright panel (Window > Enginewright, or click
 * the status bar indicator). Changes apply at once.
 */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "Enginewright"))
class ENGINEWRIGHT_API UEnginewrightSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Run the MCP server. When off, clients get no answer from this editor. */
	UPROPERTY(config, EditAnywhere, Category = "Server")
	bool bServerEnabled = true;

	/** Local port of the MCP server (http://127.0.0.1:<port>/mcp). The ENGINEWRIGHT_PORT environment variable wins over this. */
	UPROPERTY(config, EditAnywhere, Category = "Server", meta = (ClampMin = "1024", ClampMax = "65535", EditCondition = "bServerEnabled"))
	int32 Port = 3100;

	/**
	 * Refuse every tool that changes assets, the level, files or the editor (writes, builds, console commands, game runs).
	 * A guard against accidental changes, not a security boundary: an agent can turn it off again with settings_set on
	 * this class, and that call shows up in the panel's recent calls.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Tools")
	bool bReadOnly = false;

	/** Tool categories that are turned off (as listed in the panel, e.g. "UMG", "Multiplayer"). "Editor" cannot be turned off. */
	UPROPERTY(config, EditAnywhere, Category = "Tools")
	TArray<FString> DisabledCategories;

	/** How much Enginewright writes to the Output Log. */
	UPROPERTY(config, EditAnywhere, Category = "Logging")
	EEnginewrightLogLevel LogLevel = EEnginewrightLogLevel::Quiet;

	virtual FName GetContainerName() const override { return TEXT("Project"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("Enginewright"); }

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif

	/** Called after any setting changed (Project Settings, the panel or settings_set). */
	static FSimpleMulticastDelegate& OnChanged();
};
