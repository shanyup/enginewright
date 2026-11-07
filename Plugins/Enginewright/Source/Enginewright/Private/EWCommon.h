#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

#define ENGINEWRIGHT_VERSION TEXT("0.2.0-dev")

class UWorld;
class APlayerController;

namespace EW
{
	/** True while Play In Editor or Simulate runs. */
	bool IsPlaying();

	/** Short editor state: playing, map. Added to every reply. */
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

	/** Loads an asset of class T by path; OutError says why when it fails (missing, or another class). */
	template <typename T>
	T* LoadAs(const FString& Path, FString& OutError)
	{
		FString Resolved;
		UObject* Obj = ResolveObject(Path, false, Resolved, OutError);
		if (!Obj) { return nullptr; }
		T* Typed = Cast<T>(Obj);
		if (!Typed) { OutError = FString::Printf(TEXT("'%s' is a %s, not a %s."), *Path, *Obj->GetClass()->GetName(), *T::StaticClass()->GetName()); }
		return Typed;
	}

	/** Finds a class by short name (Actor, StaticMeshActor, BP_Door), /Script path or Blueprint path. */
	UClass* FindClass(const FString& Name, FString& OutError, TArray<FString>* OutSuggest = nullptr);

	/** Closest names to Wanted (case-insensitive edit distance), best first. */
	TArray<FString> Suggest(const FString& Wanted, const TArray<FString>& Candidates, int32 Max = 3);

	/** Reads a string arg; empty when missing. */
	FString Str(const TSharedPtr<FJsonObject>& Args, const FString& Key);
	/** Reads a string array arg (a single string is also accepted). */
	TArray<FString> StrList(const TSharedPtr<FJsonObject>& Args, const FString& Key);
	/** Reads a number arg with a default. */
	double Num(const TSharedPtr<FJsonObject>& Args, const FString& Key, double Default);
	bool Bool(const TSharedPtr<FJsonObject>& Args, const FString& Key, bool Default);

	/** Splits /Game/Folder/Name into package path (/Game/Folder) and asset name; checks the form. */
	bool SplitAssetPath(const FString& Path, FString& OutFolder, FString& OutName, FString& OutError);

	/** Play In Editor worlds in client order (server or standalone first). */
	TArray<UWorld*> PieWorlds();
	/** Player controller of PIE player Index (0 = first). Null with OutError when missing. */
	APlayerController* PiePlayer(int32 Index, FString& OutError);

	/** True when an asset already lives at this package path: on disk, or in memory and not deleted. */
	bool AssetPathTaken(const FString& PackagePath);

	/** Package names of assets of Class (and subclasses) under Folder, recursively. */
	TArray<FString> FindAssets(const FString& Folder, UClass* Class);

	/** Sequence number of the newest captured log line. */
	uint64 LogSeq();
	/** Captured log lines after Seq ("Category: text"), optionally of one category. */
	TArray<FString> LogLinesSince(uint64 Seq, const FString& Category = FString(), const FString& Contains = FString());

	/** Encodes RGBA8 pixels as PNG. */
	bool EncodePng(const TArray<FColor>& Pixels, int32 Width, int32 Height, TArray64<uint8>& OutPng);

	/** Runs Fn every frame (game thread) until it returns true or TimeoutSeconds pass; then calls OnTimeout. */
	void PollUntil(float TimeoutSeconds, TFunction<bool()> Fn, TFunction<void()> OnTimeout);
}
