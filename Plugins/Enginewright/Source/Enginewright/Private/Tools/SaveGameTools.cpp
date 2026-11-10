// SaveGame slots: read a slot's fields and write fields back, through the engine's reflection (USaveGame UPROPERTYs).
// A game that wraps its saves (checksum, footer, backup copy) cannot be edited by a raw write: the write would break
// the checksum. savegame_set detects such a slot and then needs the game's own save function (saveFunction).
#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EWProps.h"

#include "GameFramework/SaveGame.h"
#include "HAL/FileManager.h"
#include "JsonObjectConverter.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "PlatformFeatures.h"
#include "SaveGameSystem.h"
#include "UObject/StructOnScope.h"

namespace
{
	FEWResult Bad(const FString& Code, const FString& Msg, const TArray<FString>& Sug = {}) { return FEWResult::Error(Code, Msg, Sug); }

	ISaveGameSystem* SaveSystem()
	{
		return IPlatformFeaturesModule::Get().GetSaveGameSystem();
	}

	/** Slot names found in Saved/SaveGames (the default save location). */
	TArray<FString> ListSlots()
	{
		TArray<FString> Files, Names;
		IFileManager::Get().FindFiles(Files, *(FPaths::ProjectSavedDir() / TEXT("SaveGames/*.sav")), true, false);
		for (const FString& F : Files) { Names.Add(FPaths::GetBaseFilename(F)); }
		Names.Sort();
		return Names;
	}

	/** The fields a save game defines (all non-transient UPROPERTYs of the class and its game parents, not the engine base). */
	TArray<FString> SaveFieldNames(const UClass* C)
	{
		TArray<FString> Out;
		for (TFieldIterator<FProperty> It(C); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Transient)) { continue; }
			if (It->GetOwnerClass() == USaveGame::StaticClass() || It->GetOwnerClass() == UObject::StaticClass()) { continue; }
			Out.Add(C->GetAuthoredNameForField(*It));
		}
		return Out;
	}

	struct FSlot
	{
		FString Name;
		int32 User = 0;
		TArray<uint8> Bytes;
		USaveGame* Object = nullptr;
		int32 TrailerBytes = 0;     // bytes after the engine's own serialization (a game footer or checksum)
		bool bRoundTrips = true;    // the engine, writing this object again, produces the same bytes (apart from a trailer)
	};

	/** Loads a slot and checks whether a plain engine save would reproduce the file. */
	bool LoadSlot(const FString& Name, int32 User, FSlot& Out, FEWResult& OutErr)
	{
		Out.Name = Name; Out.User = User;
		ISaveGameSystem* SGS = SaveSystem();
		if (!SGS || !SGS->DoesSaveGameExist(*Name, User))
		{
			OutErr = Bad(TEXT("SLOT_NOT_FOUND"), FString::Printf(TEXT("No save slot '%s' for user %d."), *Name, User), EW::Suggest(Name, ListSlots()));
			if (ListSlots().Num() == 0) { OutErr.Suggestions = { TEXT("Saved/SaveGames has no .sav files; start the game once, or give class to savegame_set to create a slot.") }; }
			return false;
		}
		if (!SGS->LoadGame(false, *Name, User, Out.Bytes)) { OutErr = Bad(TEXT("LOAD_FAILED"), FString::Printf(TEXT("Could not read slot '%s'."), *Name)); return false; }
		Out.Object = UGameplayStatics::LoadGameFromMemory(Out.Bytes);
		if (!Out.Object) { OutErr = Bad(TEXT("LOAD_FAILED"), FString::Printf(TEXT("Slot '%s' (%d bytes) is not an engine save game, or its class is not loaded. If the game wraps its saves, read it with the game."), *Name, Out.Bytes.Num())); return false; }
		TArray<uint8> Again;
		if (UGameplayStatics::SaveGameToMemory(Out.Object, Again))
		{
			const bool bPrefix = Again.Num() <= Out.Bytes.Num() && FMemory::Memcmp(Again.GetData(), Out.Bytes.GetData(), Again.Num()) == 0;
			Out.bRoundTrips = bPrefix;
			Out.TrailerBytes = bPrefix ? Out.Bytes.Num() - Again.Num() : 0;
		}
		return true;
	}

	TSharedRef<FJsonObject> Fields(const USaveGame* Obj, const TArray<FString>& Names, FString& OutError, TArray<FString>& OutSug)
	{
		TSharedRef<FJsonObject> V = MakeShared<FJsonObject>();
		const UClass* C = Obj->GetClass();
		TArray<FString> Want = Names;
		if (Want.Num() == 0) { Want = SaveFieldNames(C); }
		for (const FString& N : Want)
		{
			FProperty* P = EW::FindPropertyLoose(C, N);
			if (!P) { OutError = FString::Printf(TEXT("%s has no field '%s'."), *C->GetName(), *N); OutSug = EW::Suggest(N, SaveFieldNames(C)); return V; }
			V->SetField(C->GetAuthoredNameForField(P), FJsonObjectConverter::UPropertyToJsonValue(P, P->ContainerPtrToValuePtr<void>(Obj)));
		}
		return V;
	}

	FString JsonText(const TSharedPtr<FJsonValue>& V)
	{
		FString S;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&S);
		FJsonSerializer::Serialize(V, FString(), W);
		return S;
	}
}

void RegisterSaveGameTools(FEWRegistry& R)
{
	R.Register(TEXT("savegame_get"),
		TEXT("Read a SaveGame slot from Saved/SaveGames: its class and the values of its UPROPERTY fields (all, or the names in fields). Without slot it lists the slots. Also reports whether the file carries bytes the engine did not write (customFormat, trailerBytes: a game footer or checksum), which decides whether savegame_set may write it. Read only; works with the editor idle or while Play In Editor runs. Use savegame_set to change a field."),
		TEXT(R"({"type":"object","properties":{
			"slot":{"type":"string","description":"Slot name (the file name without .sav). Omit to list the slots."},
			"userIndex":{"type":"integer","description":"Platform user index, default 0."},
			"fields":{"type":"array","items":{"type":"string"},"description":"Only these fields (UPROPERTY names). Default: all."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const FString Name = EW::Str(Args, TEXT("slot"));
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			if (Name.IsEmpty())
			{
				TArray<TSharedPtr<FJsonValue>> L;
				for (const FString& S : ListSlots())
				{
					TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
					J->SetStringField(TEXT("slot"), S);
					J->SetNumberField(TEXT("bytes"), IFileManager::Get().FileSize(*(FPaths::ProjectSavedDir() / TEXT("SaveGames") / (S + TEXT(".sav")))));
					L.Add(MakeShared<FJsonValueObject>(J));
				}
				Data->SetStringField(TEXT("folder"), FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("SaveGames")));
				Data->SetArrayField(TEXT("slots"), L);
				return FEWResult::Ok(Data);
			}
			const int32 User = (int32)EW::Num(Args, TEXT("userIndex"), 0);
			FSlot Slot; FEWResult Err;
			if (!LoadSlot(Name, User, Slot, Err)) { return Err; }
			FString E; TArray<FString> Sug;
			TSharedRef<FJsonObject> V = Fields(Slot.Object, EW::StrList(Args, TEXT("fields")), E, Sug);
			if (!E.IsEmpty()) { return Bad(TEXT("UNKNOWN_FIELD"), E, Sug); }
			Data->SetStringField(TEXT("slot"), Name);
			Data->SetStringField(TEXT("class"), Slot.Object->GetClass()->GetPathName());
			Data->SetNumberField(TEXT("bytes"), Slot.Bytes.Num());
			Data->SetBoolField(TEXT("customFormat"), Slot.TrailerBytes > 0 || !Slot.bRoundTrips);
			Data->SetNumberField(TEXT("trailerBytes"), Slot.TrailerBytes);
			Data->SetObjectField(TEXT("values"), V);
			FEWResult Res = FEWResult::Ok(Data);
			if (Slot.TrailerBytes > 0) { Res.Warnings.Add(FString::Printf(TEXT("The file has %d bytes after the engine's own data (a game checksum or footer). savegame_set will not write it without the game's save function."), Slot.TrailerBytes)); }
			return Res;
		});

	R.Register(TEXT("savegame_set"),
		TEXT("Write fields of a SaveGame slot (values: field name -> JSON value, validated against the class's UPROPERTYs; nothing is written when one value is wrong), save the slot and read it back from disk. Plain engine saves are written with SaveGameToSlot. A slot whose file has extra bytes (a game checksum or footer, see savegame_get customFormat) is refused with CUSTOM_SAVE_FORMAT, because a raw write would break the checksum: pass saveFunction, the game's own save function, which is called with the changed object and must write the slot. saveFunction is a UFUNCTION: 'ClassPathOrName:FunctionName' for a static function taking the SaveGame object, or just 'FunctionName' for a function on the SaveGame object itself with no arguments. The old file is restored when the save or the read-back fails. class creates a missing slot. Refused while Play In Editor runs, because the running game can overwrite the slot from its own copy."),
		TEXT(R"({"type":"object","properties":{
			"slot":{"type":"string","description":"Slot name (file name without .sav)."},
			"values":{"type":"object","description":"Field name -> new value, e.g. {\"bTutorialDone\": true}."},
			"userIndex":{"type":"integer","description":"Platform user index, default 0."},
			"saveFunction":{"type":"string","description":"The game's own save function: 'ClassPathOrName:FunctionName' (static UFUNCTION taking the SaveGame) or 'FunctionName' (on the SaveGame object, no arguments). Needed for slots with a game checksum or footer."},
			"class":{"type":"string","description":"SaveGame class (name or /Script path) to create the slot with when it does not exist yet."}},
			"required":["slot","values"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const FString Name = EW::Str(Args, TEXT("slot"));
			if (Name.IsEmpty() || Name.Contains(TEXT("/")) || Name.Contains(TEXT("\\")) || Name.Contains(TEXT(".."))) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("slot must be a plain slot name (no path).")); }
			const TSharedPtr<FJsonObject>* ValuesPtr = nullptr;
			if (!Args->TryGetObjectField(TEXT("values"), ValuesPtr) || !ValuesPtr || (*ValuesPtr)->Values.Num() == 0) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("values must be a non-empty object.")); }
			const TSharedPtr<FJsonObject> Values = *ValuesPtr;
			const int32 User = (int32)EW::Num(Args, TEXT("userIndex"), 0);
			const FString FuncSpec = EW::Str(Args, TEXT("saveFunction"));
			const FString ClassName = EW::Str(Args, TEXT("class"));
			ISaveGameSystem* SGS = SaveSystem();
			if (!SGS) { return Bad(TEXT("NO_SAVE_SYSTEM"), TEXT("This platform has no save game system.")); }

			FSlot Slot;
			TArray<uint8> OldBytes;
			bool bExisted = SGS->DoesSaveGameExist(*Name, User);
			if (bExisted)
			{
				FEWResult Err;
				if (!LoadSlot(Name, User, Slot, Err)) { return Err; }
				OldBytes = Slot.Bytes;
				if ((Slot.TrailerBytes > 0 || !Slot.bRoundTrips) && FuncSpec.IsEmpty())
				{
					return Bad(TEXT("CUSTOM_SAVE_FORMAT"), FString::Printf(TEXT("Slot '%s' has %d bytes the engine does not write (a game checksum or footer); a raw write would break it. Pass saveFunction with the game's own save function (a UFUNCTION), or change the value through the game."), *Name, Slot.TrailerBytes),
						{ TEXT("savegame_get shows customFormat and trailerBytes."), TEXT("saveFunction: 'ClassName:StaticFunction' (takes the SaveGame) or 'Function' (on the SaveGame object).") });
				}
			}
			else
			{
				if (ClassName.IsEmpty()) { FEWResult Err; LoadSlot(Name, User, Slot, Err); return Err; }
				FString CE; TArray<FString> CS;
				UClass* C = EW::FindClass(ClassName, CE, &CS);
				if (!C) { return Bad(TEXT("UNKNOWN_CLASS"), CE, CS); }
				if (!C->IsChildOf(USaveGame::StaticClass()) || C->HasAnyClassFlags(CLASS_Abstract)) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("%s is not a concrete SaveGame class."), *C->GetName())); }
				Slot.Object = Cast<USaveGame>(NewObject<UObject>(GetTransientPackage(), C));
			}

			// Resolve the save function before writing anything.
			UFunction* Func = nullptr;
			UObject* FuncTarget = nullptr;
			if (!FuncSpec.IsEmpty())
			{
				FString ClassPart, FuncPart;
				const bool bStatic = FuncSpec.Split(TEXT(":"), &ClassPart, &FuncPart, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
				UClass* Owner = Slot.Object->GetClass();
				if (bStatic)
				{
					FString CE; TArray<FString> CS;
					Owner = EW::FindClass(ClassPart, CE, &CS);
					if (!Owner) { return Bad(TEXT("UNKNOWN_CLASS"), CE, CS); }
				}
				else { FuncPart = FuncSpec; }
				Func = Owner->FindFunctionByName(FName(*FuncPart));
				if (!Func)
				{
					TArray<FString> Names;
					for (TFieldIterator<UFunction> It(Owner); It; ++It) { Names.Add(It->GetName()); }
					return Bad(TEXT("FUNCTION_NOT_FOUND"), FString::Printf(TEXT("%s has no UFUNCTION '%s' (only reflected functions can be called)."), *Owner->GetName(), *FuncPart), EW::Suggest(FuncPart, Names));
				}
				if (bStatic != Func->HasAnyFunctionFlags(FUNC_Static)) { return Bad(TEXT("INVALID_ARGUMENT"), bStatic ? TEXT("saveFunction names 'Class:Function' but the function is not static.") : TEXT("saveFunction without a class must be a function on the SaveGame object, but it is static; use 'Class:Function'.")); }
				int32 Params = 0; bool bObjParam = false;
				for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
				{
					if (It->HasAnyPropertyFlags(CPF_ReturnParm)) { continue; }
					++Params;
					if (const FObjectPropertyBase* OP = CastField<FObjectPropertyBase>(*It)) { bObjParam = OP->PropertyClass && Slot.Object->GetClass()->IsChildOf(OP->PropertyClass); }
				}
				if (bStatic ? !(Params == 1 && bObjParam) : Params != 0) { return Bad(TEXT("INVALID_ARGUMENT"), bStatic ? TEXT("A static saveFunction must take exactly one parameter: the SaveGame object.") : TEXT("A saveFunction on the SaveGame object must take no parameters.")); }
				FuncTarget = bStatic ? (UObject*)Owner->GetDefaultObject() : (UObject*)Slot.Object;
			}

			// Validate and write the fields (all or nothing, with read-back from the object).
			FEWResult W = EW::WriteValues(Slot.Object->GetClass(), Slot.Object, Slot.Object, Values);
			if (!W.bOk) { return W; }

			// Save.
			bool bSaved = false;
			if (Func)
			{
				FStructOnScope Parms(Func);
				for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
				{
					if (It->HasAnyPropertyFlags(CPF_ReturnParm)) { continue; }
					if (FObjectPropertyBase* OP = CastField<FObjectPropertyBase>(*It)) { OP->SetObjectPropertyValue_InContainer(Parms.GetStructMemory(), Slot.Object); }
					break;
				}
				FuncTarget->ProcessEvent(Func, Parms.GetStructMemory());
				const FBoolProperty* Ret = CastField<FBoolProperty>(Func->GetReturnProperty());
				bSaved = Ret ? Ret->GetPropertyValue_InContainer(Parms.GetStructMemory()) : true; // no bool result: judged by the read-back below
			}
			else { bSaved = UGameplayStatics::SaveGameToSlot(Slot.Object, Name, User); }
			auto Restore = [&]() { if (bExisted) { SGS->SaveGame(false, *Name, User, OldBytes); } else { SGS->DeleteGame(false, *Name, User); } };
			if (!bSaved) { Restore(); return Bad(TEXT("SAVE_FAILED"), Func ? TEXT("The save function returned false; the old file was restored.") : TEXT("SaveGameToSlot failed; the old file was restored.")); }

			// Read back from disk, compare with what was asked for.
			FSlot After; FEWResult RErr;
			if (!LoadSlot(Name, User, After, RErr)) { Restore(); return Bad(TEXT("READBACK_FAILED"), FString::Printf(TEXT("The slot could not be read back after saving (%s); the old file was restored."), *RErr.Message)); }
			TArray<FString> Keys;
			for (const auto& P : Values->Values) { Keys.Add(FString(*P.Key)); }
			FString E; TArray<FString> Sug;
			TSharedRef<FJsonObject> Back = Fields(After.Object, Keys, E, Sug);
			TArray<TSharedPtr<FJsonValue>> Mismatch;
			TSharedRef<FJsonObject> Expect = MakeShared<FJsonObject>();
			{
				FString E2; TArray<FString> S2;
				Expect = Fields(Slot.Object, Keys, E2, S2);
			}
			for (const FString& K : Keys)
			{
				const TSharedPtr<FJsonValue> B = Back->TryGetField(K); const TSharedPtr<FJsonValue> X = Expect->TryGetField(K);
				if (!B.IsValid() || !X.IsValid() || JsonText(B) != JsonText(X)) { Mismatch.Add(MakeShared<FJsonValueString>(K)); }
			}
			if (Mismatch.Num() > 0)
			{
				Restore();
				FEWResult Res = Bad(TEXT("READBACK_MISMATCH"), TEXT("The slot on disk does not hold the written values (the game's save function may have changed or refused them); the old file was restored."));
				TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
				D->SetArrayField(TEXT("fields"), Mismatch);
				D->SetObjectField(TEXT("onDisk"), Back);
				Res.Data = D;
				return Res;
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("slot"), Name);
			Data->SetStringField(TEXT("class"), After.Object->GetClass()->GetPathName());
			Data->SetStringField(TEXT("savedWith"), Func ? FuncSpec : TEXT("SaveGameToSlot"));
			Data->SetBoolField(TEXT("created"), !bExisted);
			Data->SetNumberField(TEXT("bytes"), After.Bytes.Num());
			Data->SetObjectField(TEXT("values"), Back);
			if (W.Data.IsValid() && W.Data->HasField(TEXT("changes"))) { Data->SetField(TEXT("changes"), W.Data->TryGetField(TEXT("changes"))); }
			FEWResult Res = FEWResult::Ok(Data);
			Res.Warnings = W.Warnings;
			return Res;
		},
		/*bTransactional*/ false);
}
