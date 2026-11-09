#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "EnginewrightTool.h"

class UWidgetBlueprint;

/**
 * The viewmodel_* tools live in Enginewright (schema, description, undo step, compile, save), but the code that
 * touches the MVVM plugin lives in the separate EnginewrightMVVM plugin. That plugin registers its operations here
 * while it is loaded. Without it the tools answer MVVM_NOT_ENABLED, and Enginewright never needs the MVVM plugin.
 */
struct ENGINEWRIGHT_API FEWMVVMBackend
{
	/** One operation: gets the loaded Widget Blueprint and the tool arguments, returns the result (Data may hold "changed": bool). */
	using FOp = TFunction<FEWResult(UWidgetBlueprint*, const TSharedPtr<FJsonObject>&)>;

	/** Op is "list", "add", "bind", "unbind" or "remove". */
	static void Set(const FString& Op, FOp Fn);
	/** Removes every operation (the EnginewrightMVVM module calls this on shutdown). */
	static void Clear();
	static const FOp* Find(const FString& Op);
	static bool IsAvailable();
};
