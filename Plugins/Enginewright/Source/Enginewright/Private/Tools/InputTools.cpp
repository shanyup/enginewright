#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EWProps.h"

#include "AssetToolsModule.h"
#include "EnhancedActionKeyMapping.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "IAssetTools.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectIterator.h"

namespace
{
	FEWResult Bad(const FString& Code, const FString& Msg, const TArray<FString>& Sug = {}) { return FEWResult::Error(Code, Msg, Sug); }

	bool ParseKey(const FString& Name, FKey& Out, TArray<FString>& OutSuggest)
	{
		Out = FKey(FName(*Name));
		if (Out.IsValid()) { return true; }
		TArray<FKey> All;
		EKeys::GetAllKeys(All);
		TArray<FString> Names;
		for (const FKey& K : All) { Names.Add(K.GetFName().ToString()); }
		for (const FString& N : Names) { if (N.Equals(Name, ESearchCase::IgnoreCase)) { Out = FKey(FName(*N)); return true; } }
		OutSuggest = EW::Suggest(Name, Names);
		return false;
	}

	/** Finds UInputModifier<Type> / UInputTrigger<Type> classes by short type name. */
	UClass* FindPart(UClass* Base, const FString& Prefix, const FString& Type, TArray<FString>& OutNames)
	{
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (!It->IsChildOf(Base) || It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated)) { continue; }
			FString Short = It->GetName();
			Short.RemoveFromStart(Prefix);
			OutNames.Add(Short);
			if (Short.Equals(Type, ESearchCase::IgnoreCase) || It->GetName().Equals(Type, ESearchCase::IgnoreCase)) { return *It; }
		}
		return nullptr;
	}

	/** Builds modifier/trigger objects from [{type, values}] (or plain "Type" strings), all checked first. */
	bool BuildParts(const TSharedPtr<FJsonObject>& Args, const TCHAR* Key, UClass* Base, const TCHAR* Prefix, UObject* Outer, TArray<UObject*>& Out, FEWResult& OutErr)
	{
		const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
		if (!Args->TryGetArrayField(Key, List)) { return true; }
		for (int32 i = 0; i < List->Num(); ++i)
		{
			FString Type;
			TSharedPtr<FJsonObject> Values;
			const TSharedPtr<FJsonObject>* O = nullptr;
			if ((*List)[i]->TryGetString(Type)) {}
			else if ((*List)[i]->TryGetObject(O))
			{
				(*O)->TryGetStringField(TEXT("type"), Type);
				const TSharedPtr<FJsonObject>* V = nullptr;
				if ((*O)->TryGetObjectField(TEXT("values"), V)) { Values = *V; }
				for (const auto& KV : (*O)->Values)
				{
					if (FString(*KV.Key) != TEXT("type") && FString(*KV.Key) != TEXT("values"))
					{
						OutErr = Bad(TEXT("UNKNOWN_ARGUMENT"), FString::Printf(TEXT("%s[%d] has unknown key '%s' (allowed: type, values)."), Key, i, *FString(*KV.Key)));
						return false;
					}
				}
			}
			TArray<FString> Names;
			UClass* C = FindPart(Base, Prefix, Type, Names);
			if (!C) { OutErr = Bad(TEXT("UNKNOWN_TYPE"), FString::Printf(TEXT("%s[%d]: no %s '%s'."), Key, i, Prefix, *Type), EW::Suggest(Type, Names)); return false; }
			UObject* Part = NewObject<UObject>(Outer, C, NAME_None, RF_Transactional);
			if (Values.IsValid() && Values->Values.Num())
			{
				FEWResult W = EW::WriteValues(C, Part, nullptr, Values);
				if (!W.bOk) { W.Message = FString::Printf(TEXT("%s[%d] (%s): %s"), Key, i, *Type, *W.Message); OutErr = W; return false; }
			}
			Out.Add(Part);
		}
		return true;
	}

	TSharedRef<FJsonObject> MappingJson(const FEnhancedActionKeyMapping& M)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("action"), M.Action ? M.Action->GetPathName() : FString());
		J->SetStringField(TEXT("key"), M.Key.GetFName().ToString());
		TArray<TSharedPtr<FJsonValue>> Mods, Trigs;
		for (const UInputModifier* X : M.Modifiers) { if (X) { FString S = X->GetClass()->GetName(); S.RemoveFromStart(TEXT("InputModifier")); Mods.Add(MakeShared<FJsonValueString>(S)); } }
		for (const UInputTrigger* X : M.Triggers) { if (X) { FString S = X->GetClass()->GetName(); S.RemoveFromStart(TEXT("InputTrigger")); Trigs.Add(MakeShared<FJsonValueString>(S)); } }
		J->SetArrayField(TEXT("modifiers"), Mods);
		J->SetArrayField(TEXT("triggers"), Trigs);
		return J;
	}

	bool SaveAsset(UObject* A, FEWResult& Res)
	{
		A->MarkPackageDirty();
		bool bDeferred = false;
		const bool bOk = EW::SaveOrDefer(A->GetOutermost(), bDeferred);
		if (!bOk) { Res.Warnings.Add(TEXT("Save failed; the change is in memory only.")); }
		return bOk;
	}

	UObject* CreateNew(const FString& Path, UClass* Class, FEWResult& OutErr)
	{
		FString Folder, Name, Err;
		if (!EW::SplitAssetPath(Path, Folder, Name, Err)) { OutErr = Bad(TEXT("INVALID_PATH"), Err); return nullptr; }
		if (EW::AssetPathTaken(Folder / Name)) { OutErr = Bad(TEXT("ASSET_EXISTS"), FString::Printf(TEXT("'%s' already exists."), *(Folder / Name))); return nullptr; }
		UObject* A = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().CreateAsset(Name, Folder, Class, nullptr);
		if (!A) { OutErr = Bad(TEXT("CREATE_FAILED"), TEXT("The editor could not create the asset.")); }
		return A;
	}
}

void RegisterInputTools(FEWRegistry& R)
{
	R.Register(TEXT("input_action_create"),
		TEXT("Create an Enhanced Input action asset with a value type (bool, axis1d, axis2d, axis3d)."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string","description":"e.g. /Game/Input/IA_Jump"},
			"valueType":{"type":"string","enum":["bool","axis1d","axis2d","axis3d"],"description":"Default bool."}},
			"required":["path"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UInputAction* A = Cast<UInputAction>(CreateNew(EW::Str(Args, TEXT("path")), UInputAction::StaticClass(), Err));
			if (!A) { return Err; }
			const FString T = EW::Str(Args, TEXT("valueType")).ToLower();
			A->ValueType = T == TEXT("axis1d") ? EInputActionValueType::Axis1D : T == TEXT("axis2d") ? EInputActionValueType::Axis2D : T == TEXT("axis3d") ? EInputActionValueType::Axis3D : EInputActionValueType::Boolean;
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("path"), A->GetPathName());
			Data->SetStringField(TEXT("valueType"), UEnum::GetValueAsString(A->ValueType));
			FEWResult Res = FEWResult::Ok(Data);
			Res.Data->SetBoolField(TEXT("saved"), SaveAsset(A, Res));
			return Res;
		}, /*bTransactional*/ false);

	R.Register(TEXT("input_context_create"),
		TEXT("Create an empty Enhanced Input mapping context asset (IMC)."),
		TEXT(R"({"type":"object","properties":{"path":{"type":"string","description":"e.g. /Game/Input/IMC_Default"}},"required":["path"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UInputMappingContext* C = Cast<UInputMappingContext>(CreateNew(EW::Str(Args, TEXT("path")), UInputMappingContext::StaticClass(), Err));
			if (!C) { return Err; }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("path"), C->GetPathName());
			FEWResult Res = FEWResult::Ok(Data);
			Res.Data->SetBoolField(TEXT("saved"), SaveAsset(C, Res));
			return Res;
		}, /*bTransactional*/ false);

	R.Register(TEXT("input_map"),
		TEXT("Map a key to an input action in a mapping context, with optional modifiers (Negate, SwizzleAxis, DeadZone, Scalar...) and triggers (Pressed, Released, Hold, Tap...), each as a type name or {type, values}. Everything is checked before the context changes; the mapping is read back and saved."),
		TEXT(R"({"type":"object","properties":{
			"context":{"type":"string","description":"Input Mapping Context asset path, e.g. /Game/Input/IMC_Default."},
			"action":{"type":"string","description":"Input action asset path, e.g. /Game/Input/IA_Jump."},
			"key":{"type":"string","description":"Key name, e.g. W, SpaceBar, Gamepad_LeftX."},
			"modifiers":{"type":"array","description":"Modifier type names or {type, values} objects, e.g. [\"Negate\"]."},
			"triggers":{"type":"array","description":"Trigger type names or {type, values} objects, e.g. [\"Pressed\"]."}},
			"required":["context","action","key"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Err;
			UInputMappingContext* C = EW::LoadAs<UInputMappingContext>(EW::Str(Args, TEXT("context")), Err);
			if (!C) { return Bad(TEXT("CONTEXT_NOT_FOUND"), Err); }
			UInputAction* A = EW::LoadAs<UInputAction>(EW::Str(Args, TEXT("action")), Err);
			if (!A) { return Bad(TEXT("ACTION_NOT_FOUND"), Err); }
			FKey Key;
			TArray<FString> Sug;
			if (!ParseKey(EW::Str(Args, TEXT("key")), Key, Sug)) { return Bad(TEXT("UNKNOWN_KEY"), FString::Printf(TEXT("No key '%s'."), *EW::Str(Args, TEXT("key"))), Sug); }
			for (const FEnhancedActionKeyMapping& M : C->GetMappings())
			{
				if (M.Action == A && M.Key == Key) { return Bad(TEXT("ALREADY_MAPPED"), FString::Printf(TEXT("%s is already mapped to %s in this context."), *Key.GetFName().ToString(), *A->GetName())); }
			}
			TArray<UObject*> Mods, Trigs;
			FEWResult PErr;
			if (!BuildParts(Args, TEXT("modifiers"), UInputModifier::StaticClass(), TEXT("InputModifier"), C, Mods, PErr)) { return PErr; }
			if (!BuildParts(Args, TEXT("triggers"), UInputTrigger::StaticClass(), TEXT("InputTrigger"), C, Trigs, PErr)) { return PErr; }
			C->Modify();
			FEnhancedActionKeyMapping& M = C->MapKey(A, Key);
			for (UObject* X : Mods) { M.Modifiers.Add(Cast<UInputModifier>(X)); }
			for (UObject* X : Trigs) { M.Triggers.Add(Cast<UInputTrigger>(X)); }
			const FEnhancedActionKeyMapping* Back = C->GetMappings().FindByPredicate([&](const FEnhancedActionKeyMapping& X) { return X.Action == A && X.Key == Key; });
			if (!Back || Back->Modifiers.Num() != Mods.Num() || Back->Triggers.Num() != Trigs.Num()) { return Bad(TEXT("READBACK_MISMATCH"), TEXT("The mapping did not read back as written.")); }
			FEWResult Res = FEWResult::Ok(MappingJson(*Back));
			Res.Data->SetNumberField(TEXT("mappings"), C->GetMappings().Num());
			Res.Data->SetBoolField(TEXT("saved"), SaveAsset(C, Res));
			return Res;
		}, /*bTransactional*/ false);

	R.Register(TEXT("input_unmap"),
		TEXT("Remove a key from an action in a mapping context (or every key of the action when key is omitted)."),
		TEXT(R"({"type":"object","properties":{
			"context":{"type":"string","description":"Input Mapping Context asset path, e.g. /Game/Input/IMC_Default."},
			"action":{"type":"string","description":"Input action asset path, e.g. /Game/Input/IA_Jump."},
			"key":{"type":"string","description":"Key name to remove, e.g. SpaceBar. Omit to remove every key of the action."}},
			"required":["context","action"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Err;
			UInputMappingContext* C = EW::LoadAs<UInputMappingContext>(EW::Str(Args, TEXT("context")), Err);
			if (!C) { return Bad(TEXT("CONTEXT_NOT_FOUND"), Err); }
			UInputAction* A = EW::LoadAs<UInputAction>(EW::Str(Args, TEXT("action")), Err);
			if (!A) { return Bad(TEXT("ACTION_NOT_FOUND"), Err); }
			const int32 Before = C->GetMappings().Num();
			C->Modify();
			if (Args->HasField(TEXT("key")))
			{
				FKey Key;
				TArray<FString> Sug;
				if (!ParseKey(EW::Str(Args, TEXT("key")), Key, Sug)) { return Bad(TEXT("UNKNOWN_KEY"), TEXT("No such key."), Sug); }
				if (!C->GetMappings().ContainsByPredicate([&](const FEnhancedActionKeyMapping& X) { return X.Action == A && X.Key == Key; }))
				{
					return Bad(TEXT("NOT_MAPPED"), FString::Printf(TEXT("%s is not mapped to %s here."), *Key.GetFName().ToString(), *A->GetName()));
				}
				C->UnmapKey(A, Key);
			}
			else { C->UnmapAllKeysFromAction(A); }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("removed"), Before - C->GetMappings().Num());
			FEWResult Res = FEWResult::Ok(Data);
			Res.Data->SetBoolField(TEXT("saved"), SaveAsset(C, Res));
			return Res;
		}, /*bTransactional*/ false);

	R.Register(TEXT("input_context_info"),
		TEXT("List the mappings of an Enhanced Input mapping context: action, key, modifiers, triggers."),
		TEXT(R"({"type":"object","properties":{"context":{"type":"string","description":"Input Mapping Context asset path, e.g. /Game/Input/IMC_Default."}},"required":["context"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Err;
			UInputMappingContext* C = EW::LoadAs<UInputMappingContext>(EW::Str(Args, TEXT("context")), Err);
			if (!C) { return Bad(TEXT("CONTEXT_NOT_FOUND"), Err); }
			TArray<TSharedPtr<FJsonValue>> List;
			for (const FEnhancedActionKeyMapping& M : C->GetMappings()) { List.Add(MakeShared<FJsonValueObject>(MappingJson(M))); }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("mappings"), List);
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("input_map_list"),
		TEXT("Read what keys are bound to which Enhanced Input actions, grouped by action and by device (keyboardMouse, gamepad, touch). Read only. With context (an Input Mapping Context asset) it lists that asset's mappings. Without context, while Play In Editor runs, it lists what the player really has now: every applied context (with priority) and the live mappings, runtime remaps included. Each key has its name (SpaceBar), display name, device, modifiers and triggers. Filter with action (name or path, wildcards allowed) and device. Use it to find which key an action uses when a screen hint and the game disagree (ui_prompts compares on-screen hints with these keys); input_context_info is the same for one asset without grouping."),
		TEXT(R"({"type":"object","properties":{
			"context":{"type":"string","description":"Input Mapping Context asset, e.g. /Game/Input/IMC_Default. Omit while Play In Editor runs to read the player's live mappings."},
			"action":{"type":"string","description":"Only actions whose name or path matches this (wildcards allowed), e.g. IA_Secondary or *Dash*."},
			"device":{"type":"string","enum":["keyboardMouse","gamepad","touch"],"description":"Only keys of this device."},
			"player":{"type":"integer","description":"Play In Editor player index when no context is given (default 0)."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const FString ContextArg = EW::Str(Args, TEXT("context"));
			const FString ActionFilter = EW::Str(Args, TEXT("action"));
			const FString DeviceFilter = EW::Str(Args, TEXT("device"));
			FString Err;
			TArray<FEnhancedActionKeyMapping> Maps;
			TArray<TSharedPtr<FJsonValue>> Contexts;
			FString Source;
			if (!ContextArg.IsEmpty())
			{
				UInputMappingContext* C = EW::LoadAs<UInputMappingContext>(ContextArg, Err);
				if (!C) { return Bad(TEXT("CONTEXT_NOT_FOUND"), Err); }
				Maps = C->GetMappings();
				Source = TEXT("asset");
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("context"), C->GetPathName());
				J->SetNumberField(TEXT("mappings"), Maps.Num());
				Contexts.Add(MakeShared<FJsonValueObject>(J));
			}
			else
			{
				APlayerController* PC = EW::PiePlayer((int32)EW::Num(Args, TEXT("player"), 0), Err);
				if (!PC) { return Bad(TEXT("NO_PLAYER"), TEXT("Pass context (an Input Mapping Context asset), or start Play In Editor to read the player's live mappings. ") + Err); }
				if (!EW::PlayerInputMappings(PC, Maps)) { return Bad(TEXT("NO_ENHANCED_INPUT"), TEXT("This player has no Enhanced Input player input (the game uses legacy input).")); }
				Source = TEXT("player");
				const TArray<TPair<FString, int32>> Applied = EW::PlayerInputContexts(PC);
				for (const TPair<FString, int32>& A : Applied)
				{
					TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
					J->SetStringField(TEXT("context"), A.Key);
					J->SetNumberField(TEXT("priority"), A.Value);
					Contexts.Add(MakeShared<FJsonValueObject>(J));
				}
			}

			TArray<FString> Order;
			TMap<FString, TArray<TSharedPtr<FJsonValue>>> KeysOf;
			TMap<FString, TMap<FString, TArray<FString>>> ByDevice;
			TMap<FString, const UInputAction*> Actions;
			int32 KeyCount = 0;
			for (const FEnhancedActionKeyMapping& M : Maps)
			{
				if (!M.Action) { continue; }
				const FString Path = M.Action->GetPathName();
				if (ActionFilter.Len() && !M.Action->GetName().Equals(ActionFilter, ESearchCase::IgnoreCase) && !M.Action->GetName().MatchesWildcard(ActionFilter) && !Path.Equals(ActionFilter, ESearchCase::IgnoreCase) && !FPackageName::ObjectPathToPackageName(Path).Equals(ActionFilter, ESearchCase::IgnoreCase) && !Path.MatchesWildcard(ActionFilter)) { continue; }
				const FString Device = M.Key.IsGamepadKey() ? TEXT("gamepad") : M.Key.IsTouch() ? TEXT("touch") : TEXT("keyboardMouse");
				if (DeviceFilter.Len() && DeviceFilter != Device) { continue; }
				if (!KeysOf.Contains(Path)) { Order.Add(Path); Actions.Add(Path, M.Action); }
				TSharedRef<FJsonObject> K = MappingJson(M);
				K->RemoveField(TEXT("action"));
				K->SetStringField(TEXT("display"), M.Key.GetDisplayName(false).ToString());
				K->SetStringField(TEXT("device"), Device);
				KeysOf.FindOrAdd(Path).Add(MakeShared<FJsonValueObject>(K));
				ByDevice.FindOrAdd(Path).FindOrAdd(Device).AddUnique(M.Key.GetFName().ToString());
				++KeyCount;
			}
			TArray<TSharedPtr<FJsonValue>> List;
			for (const FString& Path : Order)
			{
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("action"), Path);
				J->SetStringField(TEXT("name"), Actions[Path]->GetName());
				J->SetStringField(TEXT("valueType"), UEnum::GetValueAsString(Actions[Path]->ValueType));
				TSharedRef<FJsonObject> Dev = MakeShared<FJsonObject>();
				for (const TPair<FString, TArray<FString>>& D : ByDevice[Path])
				{
					TArray<TSharedPtr<FJsonValue>> Names;
					for (const FString& N : D.Value) { Names.Add(MakeShared<FJsonValueString>(N)); }
					Dev->SetArrayField(D.Key, Names);
				}
				J->SetObjectField(TEXT("byDevice"), Dev);
				J->SetArrayField(TEXT("keys"), KeysOf[Path]);
				List.Add(MakeShared<FJsonValueObject>(J));
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("source"), Source);
			Data->SetArrayField(TEXT("contexts"), Contexts);
			Data->SetArrayField(TEXT("actions"), List);
			Data->SetNumberField(TEXT("actionCount"), List.Num());
			Data->SetNumberField(TEXT("keyCount"), KeyCount);
			FEWResult Res = FEWResult::Ok(Data);
			if (List.Num() == 0) { Res.Warnings.Add(Source == TEXT("player") && Maps.Num() == 0 ? TEXT("The player has no live Enhanced Input mappings (no mapping context is applied).") : TEXT("No mapping matched the action / device filter.")); }
			return Res;
		});
}
