#include "EWProps.h"
#include "EWCommon.h"

#include "FileHelpers.h"
#include "JsonObjectConverter.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace EW
{
	FProperty* FindPropertyLoose(const UStruct* Struct, const FString& Key)
	{
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (It->GetName().Equals(Key, ESearchCase::IgnoreCase) ||
				Struct->GetAuthoredNameForField(*It).Equals(Key, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
		return nullptr;
	}

	TArray<FString> PropertyNames(const UStruct* Struct)
	{
		TArray<FString> Names;
		for (TFieldIterator<FProperty> It(Struct); It; ++It) { Names.Add(Struct->GetAuthoredNameForField(*It)); }
		return Names;
	}

	static bool IsRefProperty(const FProperty* P)
	{
		return P->IsA<FObjectPropertyBase>() || P->IsA<FSoftObjectProperty>() || P->IsA<FSoftClassProperty>();
	}

	/**
	 * Checks a JSON value against a property before writing.
	 * - Unknown struct fields are errors (the engine converter ignores them silently).
	 * - {refPath:"..."} / {path:"..."} for object and soft references becomes the plain path string.
	 * Returns the normalized value, or null with OutError set.
	 */
	static TSharedPtr<FJsonValue> Normalize(const TSharedPtr<FJsonValue>& V, FProperty* P, const FString& Where, FString& OutError, TArray<FString>& OutSuggest)
	{
		if (!V.IsValid()) { return V; }

		if (IsRefProperty(P) && V->Type == EJson::Object)
		{
			FString Ref;
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (O->TryGetStringField(TEXT("refPath"), Ref) || O->TryGetStringField(TEXT("path"), Ref) || O->TryGetStringField(TEXT("objectPath"), Ref))
			{
				return MakeShared<FJsonValueString>(Ref);
			}
			OutError = FString::Printf(TEXT("%s: a reference takes a path string like /Game/X/BP_Y.BP_Y_C."), *Where);
			return nullptr;
		}

		if (FStructProperty* SP = CastField<FStructProperty>(P))
		{
			if (V->Type != EJson::Object) { return V; } // Let the engine parse text forms (e.g. vectors).
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			for (const auto& Pair : V->AsObject()->Values)
			{
				const FString Key(*Pair.Key);
				FProperty* Inner = FindPropertyLoose(SP->Struct, Key);
				if (!Inner)
				{
					OutError = FString::Printf(TEXT("%s: struct %s has no field '%s'."), *Where, *SP->Struct->GetName(), *Key);
					OutSuggest = Suggest(Key, PropertyNames(SP->Struct));
					return nullptr;
				}
				TSharedPtr<FJsonValue> N = Normalize(Pair.Value, Inner, Where + TEXT(".") + Key, OutError, OutSuggest);
				if (!N.IsValid() && !OutError.IsEmpty()) { return nullptr; }
				Out->SetField(SP->Struct->GetAuthoredNameForField(Inner), N);
			}
			return MakeShared<FJsonValueObject>(Out);
		}

		if (FArrayProperty* AP = CastField<FArrayProperty>(P))
		{
			if (V->Type != EJson::Array) { return V; }
			TArray<TSharedPtr<FJsonValue>> Out;
			const TArray<TSharedPtr<FJsonValue>>& In = V->AsArray();
			for (int32 i = 0; i < In.Num(); ++i)
			{
				TSharedPtr<FJsonValue> N = Normalize(In[i], AP->Inner, FString::Printf(TEXT("%s[%d]"), *Where, i), OutError, OutSuggest);
				if (!N.IsValid() && !OutError.IsEmpty()) { return nullptr; }
				Out.Add(N);
			}
			return MakeShared<FJsonValueArray>(Out);
		}

		// Scalars: the engine converter turns wrong types into 0/false silently, so check them here.
		if (CastField<FBoolProperty>(P))
		{
			if (V->Type != EJson::Boolean)
			{
				OutError = FString::Printf(TEXT("%s is a bool; send true or false."), *Where);
				return nullptr;
			}
			return V;
		}
		const UEnum* Enum = nullptr;
		if (FEnumProperty* EP = CastField<FEnumProperty>(P)) { Enum = EP->GetEnum(); }
		else if (FByteProperty* ByP = CastField<FByteProperty>(P)) { Enum = ByP->Enum; }
		if (Enum)
		{
			if (V->Type == EJson::String)
			{
				const FString Name = V->AsString();
				const FString Short = Name.Contains(TEXT("::")) ? Name.RightChop(Name.Find(TEXT("::")) + 2) : Name;
				if (Enum->GetValueByNameString(Short) == INDEX_NONE && Enum->GetValueByNameString(Name) == INDEX_NONE)
				{
					TArray<FString> Names;
					for (int32 i = 0; i < Enum->NumEnums() - 1; ++i) { Names.Add(Enum->GetNameStringByIndex(i)); }
					OutError = FString::Printf(TEXT("%s: '%s' is not a value of %s."), *Where, *Name, *Enum->GetName());
					OutSuggest = Suggest(Short, Names);
					return nullptr;
				}
				return MakeShared<FJsonValueString>(Short);
			}
			if (V->Type != EJson::Number)
			{
				OutError = FString::Printf(TEXT("%s is an enum %s; send its name as a string."), *Where, *Enum->GetName());
				return nullptr;
			}
			return V;
		}
		if (CastField<FNumericProperty>(P))
		{
			if (V->Type == EJson::Number) { return V; }
			if (V->Type == EJson::String && V->AsString().IsNumeric()) { return MakeShared<FJsonValueNumber>(FCString::Atod(*V->AsString())); }
			OutError = FString::Printf(TEXT("%s is a number; got %s."), *Where, V->Type == EJson::String ? *(TEXT("'") + V->AsString() + TEXT("'")) : TEXT("a non-number"));
			return nullptr;
		}
		if ((P->IsA<FStrProperty>() || P->IsA<FNameProperty>() || P->IsA<FTextProperty>()) && V->Type != EJson::String && V->Type != EJson::Null)
		{
			OutError = FString::Printf(TEXT("%s is text; send a string."), *Where);
			return nullptr;
		}

		return V;
	}

	static FString ValueToText(const TSharedPtr<FJsonValue>& V)
	{
		if (!V.IsValid()) { return TEXT("null"); }
		TSharedRef<FJsonObject> Wrap = MakeShared<FJsonObject>();
		Wrap->SetField(TEXT("v"), V);
		return ToJsonString(Wrap);
	}

	FBatchState& Batch()
	{
		static FBatchState State;
		return State;
	}

	void CancelUnlessBatch(FScopedTransaction& Tx)
	{
		if (Batch().Depth == 0) { Tx.Cancel(); }
	}

	bool SaveOrDefer(UPackage* Package, bool& bOutDeferred)
	{
		bOutDeferred = false;
		if (!Package) { return false; }
		if (Batch().Depth > 0)
		{
			Batch().Pending.AddUnique(Package);
			bOutDeferred = true;
			return true;
		}
		return UEditorLoadingAndSavingUtils::SavePackages({ Package }, false);
	}

	FEWResult ReadValues(const UStruct* Struct, const void* Container, const TArray<FString>& Names, TSharedPtr<FJsonObject>& OutValues)
	{
		OutValues = MakeShared<FJsonObject>();
		if (Names.Num() == 0)
		{
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				if (Struct->IsA<UClass>() && !It->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible | CPF_Config)) { continue; }
				OutValues->SetField(Struct->GetAuthoredNameForField(*It), FJsonObjectConverter::UPropertyToJsonValue(*It, It->ContainerPtrToValuePtr<void>(Container)));
			}
			return FEWResult::Ok();
		}
		for (const FString& Name : Names)
		{
			FProperty* P = FindPropertyLoose(Struct, Name);
			if (!P)
			{
				return FEWResult::Error(TEXT("UNKNOWN_PROPERTY"), FString::Printf(TEXT("%s has no property '%s'."), *Struct->GetName(), *Name),
					Suggest(Name, PropertyNames(Struct)));
			}
			OutValues->SetField(Struct->GetAuthoredNameForField(P), FJsonObjectConverter::UPropertyToJsonValue(P, P->ContainerPtrToValuePtr<void>(Container)));
		}
		return FEWResult::Ok();
	}

	FEWResult WriteValues(const UStruct* Struct, void* Container, UObject* Owner, const TSharedPtr<FJsonObject>& Values)
	{
		if (!Values.IsValid() || Values->Values.Num() == 0)
		{
			return FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("values must be a non-empty object."));
		}

		// 1) Validate everything first. Nothing is written if one value is wrong.
		TArray<TPair<FProperty*, TSharedPtr<FJsonValue>>> Plan;
		for (const auto& Pair : Values->Values)
		{
			const FString Key(*Pair.Key);
			FProperty* P = FindPropertyLoose(Struct, Key);
			if (!P)
			{
				return FEWResult::Error(TEXT("UNKNOWN_PROPERTY"),
					FString::Printf(TEXT("%s has no property '%s'. Nothing was written."), *Struct->GetName(), *Key),
					Suggest(Key, PropertyNames(Struct)));
			}
			FString NErr;
			TArray<FString> NSug;
			TSharedPtr<FJsonValue> N = Normalize(Pair.Value, P, P->GetName(), NErr, NSug);
			if (!NErr.IsEmpty())
			{
				return FEWResult::Error(TEXT("INVALID_VALUE"), NErr + TEXT(" Nothing was written."), NSug);
			}
			Plan.Emplace(P, N);
		}

		// 2) Write. Roll back the values already written on the first failure.
		if (Owner) { Owner->Modify(); }
		TArray<FString> Before;
		for (const auto& Step : Plan)
		{
			Before.Add(ValueToText(FJsonObjectConverter::UPropertyToJsonValue(Step.Key, Step.Key->ContainerPtrToValuePtr<void>(Container))));
		}
		for (int32 i = 0; i < Plan.Num(); ++i)
		{
			FProperty* P = Plan[i].Key;
			FText Fail;
			if (!FJsonObjectConverter::JsonValueToUProperty(Plan[i].Value, P, P->ContainerPtrToValuePtr<void>(Container), 0, 0, false, &Fail))
			{
				for (int32 j = 0; j <= i; ++j)
				{
					TSharedPtr<FJsonObject> W = ParseObject(Before[j]);
					if (W.IsValid()) { FJsonObjectConverter::JsonValueToUProperty(W->TryGetField(TEXT("v")), Plan[j].Key, Plan[j].Key->ContainerPtrToValuePtr<void>(Container)); }
				}
				return FEWResult::Error(TEXT("WRITE_FAILED"),
					FString::Printf(TEXT("Could not write %s: %s. All changes of this call were rolled back."), *P->GetName(), *Fail.ToString()));
			}
			if (Owner)
			{
				FPropertyChangedEvent Ev(P, EPropertyChangeType::ValueSet);
				Owner->PostEditChangeProperty(Ev);
			}
		}

		// 3) Read back and compare.
		FEWResult Res = FEWResult::Ok();
		TSharedRef<FJsonObject> Changes = MakeShared<FJsonObject>();
		for (int32 i = 0; i < Plan.Num(); ++i)
		{
			FProperty* P = Plan[i].Key;
			const TSharedPtr<FJsonValue> After = FJsonObjectConverter::UPropertyToJsonValue(P, P->ContainerPtrToValuePtr<void>(Container));
			const FString AfterText = ValueToText(After);
			TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
			TSharedPtr<FJsonObject> B = ParseObject(Before[i]);
			C->SetField(TEXT("before"), B.IsValid() ? B->TryGetField(TEXT("v")) : nullptr);
			C->SetField(TEXT("after"), After);
			Changes->SetObjectField(Struct->GetAuthoredNameForField(P), C);
			if (Before[i] == AfterText)
			{
				Res.Warnings.Add(FString::Printf(TEXT("%s did not change (same value, or the engine ignored it)."), *P->GetName()));
			}
			const TSharedPtr<FJsonValue>& Sent = Plan[i].Value;
			if (IsRefProperty(P) && Sent.IsValid() && Sent->Type == EJson::String && !Sent->AsString().IsEmpty() &&
				Sent->AsString() != TEXT("None") && AfterText.Contains(TEXT("\"None\"")))
			{
				// Undo this field so a bad reference never stays half-applied.
				TSharedPtr<FJsonObject> W = ParseObject(Before[i]);
				if (W.IsValid()) { FJsonObjectConverter::JsonValueToUProperty(W->TryGetField(TEXT("v")), P, P->ContainerPtrToValuePtr<void>(Container)); }
				Res.bOk = false;
				Res.ErrorCode = TEXT("REFERENCE_NOT_SET");
				Res.Message = FString::Printf(TEXT("%s stayed empty after writing '%s'. Check the path (class paths end in _C)."), *P->GetName(), *Sent->AsString());
			}
		}
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetObjectField(TEXT("changes"), Changes);
		Res.Data = Data;
		return Res;
	}
}
