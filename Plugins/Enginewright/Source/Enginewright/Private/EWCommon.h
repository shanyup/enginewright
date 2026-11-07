#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace EW
{
	/** True while Play In Editor or Simulate runs. */
	bool IsPlaying();

	/** Short editor state: playing, compiling, map. Added to every reply. */
	TSharedRef<FJsonObject> EditorState();

	/** Parses a JSON object from text. Returns null on bad JSON. */
	TSharedPtr<FJsonObject> ParseObject(const FString& Text);
	FString ToJsonString(const TSharedRef<FJsonObject>& Obj);

	/**
	 * Resolves an object from any of these forms:
	 * /Game/X, /Game/X.X, /Game/X.X_C (class), Default__ / :Component sub-objects.
	 * bClassDefault: when the path names a Blueprint or class, return its class default object.
	 */
	UObject* ResolveObject(const FString& Path, bool bClassDefault, FString& OutResolvedPath, FString& OutError);

	/** Closest names to Wanted (case-insensitive edit distance), best first. */
	TArray<FString> Suggest(const FString& Wanted, const TArray<FString>& Candidates, int32 Max = 3);

	/** Reads a string arg; empty when missing. */
	FString Str(const TSharedPtr<FJsonObject>& Args, const FString& Key);
}
