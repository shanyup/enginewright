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
	/** Images sent to the client as MCP image content: (mime type, base64 data). */
	TArray<TPair<FString, FString>> Images;

	static FEWResult Ok(TSharedPtr<FJsonObject> InData = nullptr, const FString& InMessage = FString());
	static FEWResult Error(const FString& InCode, const FString& InMessage, const TArray<FString>& InSuggestions = {});

	TSharedRef<FJsonObject> ToJson() const;
};

/** Completion callback of an asynchronous tool. Called once, on the game thread. */
using FEWDone = TFunction<void(FEWResult)>;

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
	/** Set instead of Run for tools that finish on a later frame (log wait, input hold, tests). */
	TFunction<void(const TSharedPtr<FJsonObject>& Args, FEWDone Done)> RunAsync;

	bool IsAsync() const { return (bool)RunAsync; }
};

class ENGINEWRIGHT_API FEWRegistry
{
public:
	static FEWRegistry& Get();

	/** SchemaJson is a JSON Schema object as text. */
	void Register(const FString& Name, const FString& Description, const FString& SchemaJson, bool bWrites,
		TFunction<FEWResult(const TSharedPtr<FJsonObject>&)> Run, bool bTransactional = true);

	/** Registers a tool that finishes on a later frame. Async tools cannot run inside batch. */
	void RegisterAsync(const FString& Name, const FString& Description, const FString& SchemaJson, bool bWrites,
		TFunction<void(const TSharedPtr<FJsonObject>&, FEWDone)> Run);

	const FEWTool* Find(const FString& Name) const;
	const TArray<FEWTool>& All() const { return Tools; }

	/** Runs a synchronous tool with the shared checks (schema args and types, PIE gate). */
	FEWResult Call(const FString& Name, const TSharedPtr<FJsonObject>& Args);

	/** Runs any tool; Done is called at once for synchronous tools. */
	void CallAsync(const FString& Name, const TSharedPtr<FJsonObject>& Args, FEWDone Done);

	/** The shared checks only. Returns ok with OutTool set, or the error. */
	FEWResult Check(const FString& Name, const TSharedPtr<FJsonObject>& Args, const FEWTool*& OutTool) const;

private:
	TArray<FEWTool> Tools;
};
