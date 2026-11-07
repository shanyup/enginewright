#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "EnginewrightTool.h"

class UPackage;
class FScopedTransaction;

namespace EW
{
	FProperty* FindPropertyLoose(const UStruct* Struct, const FString& Key);
	TArray<FString> PropertyNames(const UStruct* Struct);

	/** Reads properties of a struct instance (all editable ones when Names is empty). Fails on unknown names. */
	FEWResult ReadValues(const UStruct* Struct, const void* Container, const TArray<FString>& Names, TSharedPtr<FJsonObject>& OutValues);

	/**
	 * Validates every value first (unknown property / struct field, type, enum, reference form),
	 * then writes all of them, rolls back on the first engine failure, reads each one back and compares.
	 * Owner (optional) gets Modify() and PostEditChangeProperty. Data of the result holds "changes".
	 */
	FEWResult WriteValues(const UStruct* Struct, void* Container, UObject* Owner, const TSharedPtr<FJsonObject>& Values);

	/** Saves the package now, or defers it to the end of the running batch. Returns false only on a failed save. */
	bool SaveOrDefer(UPackage* Package, bool& bOutDeferred);

	/** Batch state: while Depth > 0 saves are deferred and collected. */
	struct FBatchState
	{
		int32 Depth = 0;
		TArray<TWeakObjectPtr<UPackage>> Pending;
	};
	FBatchState& Batch();

	/** Cancels the transaction, except inside a batch (the batch owns the undo step and rolls back as a whole). */
	void CancelUnlessBatch(FScopedTransaction& Tx);
}
