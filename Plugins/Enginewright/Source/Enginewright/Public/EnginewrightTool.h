#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Result of one tool call. Every call says clearly if it worked. */
struct ENGINEWRIGHT_API FEWResult
{
	bool bOk = true;
	FString ErrorCode;
	FString Message;
	TSharedPtr<FJsonObject> Data;
	TArray<FString> Warnings;
	TArray<FString> Suggestions;

	static FEWResult Ok(TSharedPtr<FJsonObject> InData = nullptr, const FString& InMessage = FString());
	static FEWResult Error(const FString& InCode, const FString& InMessage, const TArray<FString>& InSuggestions = {});

	TSharedRef<FJsonObject> ToJson() const;
};

/** One tool. bWrites = it changes assets or the level (blocked during Play In Editor). */
struct ENGINEWRIGHT_API FEWTool
{
	FString Name;
	FString Description;
	TSharedPtr<FJsonObject> InputSchema;
	bool bWrites = false;
	/** False for steps an editor undo cannot revert (asset create/rename/delete, saves). */
	bool bTransactional = true;
	TFunction<FEWResult(const TSharedPtr<FJsonObject>& Args)> Run;
};

class ENGINEWRIGHT_API FEWRegistry
{
public:
	static FEWRegistry& Get();

	/** SchemaJson is a JSON Schema object as text. */
	void Register(const FString& Name, const FString& Description, const FString& SchemaJson, bool bWrites,
		TFunction<FEWResult(const TSharedPtr<FJsonObject>&)> Run, bool bTransactional = true);

	const FEWTool* Find(const FString& Name) const;
	const TArray<FEWTool>& All() const { return Tools; }

	/** Runs a tool with the shared checks: schema-required args, PIE gate, warnings. */
	FEWResult Call(const FString& Name, const TSharedPtr<FJsonObject>& Args);

private:
	TArray<FEWTool> Tools;
};
