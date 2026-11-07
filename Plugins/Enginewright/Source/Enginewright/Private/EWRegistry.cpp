#include "EnginewrightTool.h"
#include "EWCommon.h"

FEWResult FEWResult::Ok(TSharedPtr<FJsonObject> InData, const FString& InMessage)
{
	FEWResult R;
	R.bOk = true;
	R.Data = InData.IsValid() ? InData : MakeShared<FJsonObject>();
	R.Message = InMessage;
	return R;
}

FEWResult FEWResult::Error(const FString& InCode, const FString& InMessage, const TArray<FString>& InSuggestions)
{
	FEWResult R;
	R.bOk = false;
	R.ErrorCode = InCode;
	R.Message = InMessage;
	R.Suggestions = InSuggestions;
	return R;
}

static TArray<TSharedPtr<FJsonValue>> ToJsonArray(const TArray<FString>& In)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FString& S : In) { Out.Add(MakeShared<FJsonValueString>(S)); }
	return Out;
}

TSharedRef<FJsonObject> FEWResult::ToJson() const
{
	TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetBoolField(TEXT("ok"), bOk);
	if (!Message.IsEmpty()) { J->SetStringField(TEXT("message"), Message); }
	if (!bOk) { J->SetStringField(TEXT("errorCode"), ErrorCode); }
	if (Data.IsValid()) { J->SetObjectField(TEXT("data"), Data); }
	if (Warnings.Num()) { J->SetArrayField(TEXT("warnings"), ToJsonArray(Warnings)); }
	if (Suggestions.Num()) { J->SetArrayField(TEXT("suggestions"), ToJsonArray(Suggestions)); }
	J->SetObjectField(TEXT("editor"), EW::EditorState());
	return J;
}

FEWRegistry& FEWRegistry::Get()
{
	static FEWRegistry Instance;
	return Instance;
}

void FEWRegistry::Register(const FString& Name, const FString& Description, const FString& SchemaJson, bool bWrites,
	TFunction<FEWResult(const TSharedPtr<FJsonObject>&)> Run, bool bTransactional)
{
	FEWTool T;
	T.Name = Name;
	T.Description = Description;
	T.InputSchema = EW::ParseObject(SchemaJson);
	if (!T.InputSchema.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Enginewright: bad schema JSON for tool %s"), *Name);
		T.InputSchema = MakeShared<FJsonObject>();
		T.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
	}
	T.bWrites = bWrites;
	T.bTransactional = bTransactional;
	T.Run = MoveTemp(Run);
	Tools.Add(MoveTemp(T));
}

const FEWTool* FEWRegistry::Find(const FString& Name) const
{
	return Tools.FindByPredicate([&Name](const FEWTool& T) { return T.Name.Equals(Name, ESearchCase::IgnoreCase); });
}

FEWResult FEWRegistry::Call(const FString& Name, const TSharedPtr<FJsonObject>& InArgs)
{
	const FEWTool* Tool = Find(Name);
	if (!Tool)
	{
		TArray<FString> Names;
		for (const FEWTool& T : Tools) { Names.Add(T.Name); }
		return FEWResult::Error(TEXT("UNKNOWN_TOOL"), FString::Printf(TEXT("No tool named '%s'. Use search."), *Name),
			EW::Suggest(Name, Names));
	}

	TSharedPtr<FJsonObject> Args = InArgs.IsValid() ? InArgs : MakeShared<FJsonObject>();

	// Strict args: unknown keys and missing required keys are errors, never silently ignored.
	TArray<FString> Allowed;
	const TSharedPtr<FJsonObject>* Props = nullptr;
	if (Tool->InputSchema->TryGetObjectField(TEXT("properties"), Props))
	{
		for (const auto& P : (*Props)->Values) { Allowed.Add(FString(*P.Key)); }
	}
	for (const auto& Pair : Args->Values)
	{
		const FString Key(*Pair.Key);
		if (!Allowed.Contains(Key))
		{
			return FEWResult::Error(TEXT("UNKNOWN_ARGUMENT"),
				FString::Printf(TEXT("'%s' does not take argument '%s'. Allowed: %s"), *Tool->Name, *Key, *FString::Join(Allowed, TEXT(", "))),
				EW::Suggest(Key, Allowed));
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
	if (Tool->InputSchema->TryGetArrayField(TEXT("required"), Required))
	{
		for (const TSharedPtr<FJsonValue>& R : *Required)
		{
			if (!Args->HasField(R->AsString()))
			{
				return FEWResult::Error(TEXT("MISSING_ARGUMENT"),
					FString::Printf(TEXT("'%s' needs argument '%s'."), *Tool->Name, *R->AsString()));
			}
		}
	}

	// PIE gate: edits during Play In Editor are lost or corrupt assets.
	if (Tool->bWrites && EW::IsPlaying())
	{
		return FEWResult::Error(TEXT("PIE_RUNNING"),
			FString::Printf(TEXT("'%s' changes assets or the level and is blocked while Play In Editor runs. Stop Play first (pie_stop)."), *Tool->Name));
	}

	return Tool->Run(Args);
}
