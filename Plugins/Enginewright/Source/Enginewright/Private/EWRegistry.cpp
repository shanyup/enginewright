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

static TSharedPtr<FJsonObject> ParseSchema(const FString& Name, const FString& SchemaJson)
{
	TSharedPtr<FJsonObject> S = EW::ParseObject(SchemaJson);
	if (!S.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Enginewright: bad schema JSON for tool %s"), *Name);
		S = MakeShared<FJsonObject>();
		S->SetStringField(TEXT("type"), TEXT("object"));
	}
	return S;
}

void FEWRegistry::Register(const FString& Name, const FString& Description, const FString& SchemaJson, bool bWrites,
	TFunction<FEWResult(const TSharedPtr<FJsonObject>&)> Run, bool bTransactional)
{
	FEWTool T;
	T.Name = Name;
	T.Description = Description;
	T.InputSchema = ParseSchema(Name, SchemaJson);
	T.bWrites = bWrites;
	T.bTransactional = bTransactional;
	T.Run = MoveTemp(Run);
	Tools.Add(MoveTemp(T));
}

void FEWRegistry::RegisterAsync(const FString& Name, const FString& Description, const FString& SchemaJson, bool bWrites,
	TFunction<void(const TSharedPtr<FJsonObject>&, FEWDone)> Run)
{
	FEWTool T;
	T.Name = Name;
	T.Description = Description;
	T.InputSchema = ParseSchema(Name, SchemaJson);
	T.bWrites = bWrites;
	T.bTransactional = false;
	T.RunAsync = MoveTemp(Run);
	Tools.Add(MoveTemp(T));
}

const FEWTool* FEWRegistry::Find(const FString& Name) const
{
	return Tools.FindByPredicate([&Name](const FEWTool& T) { return T.Name.Equals(Name, ESearchCase::IgnoreCase); });
}

namespace
{
	const TCHAR* JsonTypeName(EJson T)
	{
		switch (T)
		{
		case EJson::String: return TEXT("string");
		case EJson::Number: return TEXT("number");
		case EJson::Boolean: return TEXT("boolean");
		case EJson::Array: return TEXT("array");
		case EJson::Object: return TEXT("object");
		case EJson::Null: return TEXT("null");
		default: return TEXT("unknown");
		}
	}

	/** True when the JSON value fits the schema type (a list like ["string","array"] is allowed). */
	bool TypeFits(const TSharedPtr<FJsonValue>& V, const FString& Want)
	{
		if (Want.IsEmpty()) { return true; }
		switch (V->Type)
		{
		case EJson::String: return Want == TEXT("string");
		case EJson::Boolean: return Want == TEXT("boolean");
		case EJson::Array: return Want == TEXT("array");
		case EJson::Object: return Want == TEXT("object");
		case EJson::Number:
			if (Want == TEXT("number")) { return true; }
			if (Want == TEXT("integer")) { return FMath::IsNearlyEqual(V->AsNumber(), FMath::RoundToDouble(V->AsNumber())); }
			return false;
		default: return false;
		}
	}
}

FEWResult FEWRegistry::Check(const FString& Name, const TSharedPtr<FJsonObject>& Args, const FEWTool*& OutTool) const
{
	OutTool = nullptr;
	const FEWTool* Tool = Find(Name);
	if (!Tool)
	{
		TArray<FString> Names;
		for (const FEWTool& T : Tools) { Names.Add(T.Name); }
		return FEWResult::Error(TEXT("UNKNOWN_TOOL"), FString::Printf(TEXT("No tool named '%s'. Use search."), *Name),
			EW::Suggest(Name, Names));
	}

	// Strict args: unknown keys, missing required keys and wrong JSON types are errors, never silently ignored.
	TArray<FString> Allowed;
	const TSharedPtr<FJsonObject>* Props = nullptr;
	Tool->InputSchema->TryGetObjectField(TEXT("properties"), Props);
	if (Props)
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
		const TSharedPtr<FJsonObject>* PropSchema = nullptr;
		if (!(*Props)->TryGetObjectField(Key, PropSchema) || !Pair.Value.IsValid() || Pair.Value->IsNull()) { continue; }
		TArray<FString> Types;
		FString OneType;
		const TArray<TSharedPtr<FJsonValue>>* TypeList = nullptr;
		if ((*PropSchema)->TryGetStringField(TEXT("type"), OneType)) { Types.Add(OneType); }
		else if ((*PropSchema)->TryGetArrayField(TEXT("type"), TypeList)) { for (const auto& T : *TypeList) { Types.Add(T->AsString()); } }
		if (Types.Num() && !Types.ContainsByPredicate([&](const FString& T) { return TypeFits(Pair.Value, T); }))
		{
			return FEWResult::Error(TEXT("INVALID_ARGUMENT"),
				FString::Printf(TEXT("'%s' argument '%s' must be %s, got %s."), *Tool->Name, *Key, *FString::Join(Types, TEXT(" or ")), JsonTypeName(Pair.Value->Type)));
		}
		const TArray<TSharedPtr<FJsonValue>>* Enum = nullptr;
		if (Pair.Value->Type == EJson::String && (*PropSchema)->TryGetArrayField(TEXT("enum"), Enum))
		{
			TArray<FString> Options;
			for (const auto& E : *Enum) { Options.Add(E->AsString()); }
			const FString Got = Pair.Value->AsString();
			if (!Options.ContainsByPredicate([&](const FString& O) { return O.Equals(Got, ESearchCase::IgnoreCase); }))
			{
				return FEWResult::Error(TEXT("INVALID_ARGUMENT"),
					FString::Printf(TEXT("'%s' argument '%s' must be one of: %s."), *Tool->Name, *Key, *FString::Join(Options, TEXT(", "))),
					EW::Suggest(Got, Options));
			}
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

	OutTool = Tool;
	return FEWResult::Ok();
}

FEWResult FEWRegistry::Call(const FString& Name, const TSharedPtr<FJsonObject>& InArgs)
{
	TSharedPtr<FJsonObject> Args = InArgs.IsValid() ? InArgs : MakeShared<FJsonObject>();
	const FEWTool* Tool = nullptr;
	FEWResult Checked = Check(Name, Args, Tool);
	if (!Tool) { return Checked; }
	if (Tool->IsAsync())
	{
		return FEWResult::Error(TEXT("ASYNC_TOOL"),
			FString::Printf(TEXT("'%s' waits on later frames and cannot run inside batch. Call it on its own."), *Tool->Name));
	}
	return Tool->Run(Args);
}

void FEWRegistry::CallAsync(const FString& Name, const TSharedPtr<FJsonObject>& InArgs, FEWDone Done)
{
	TSharedPtr<FJsonObject> Args = InArgs.IsValid() ? InArgs : MakeShared<FJsonObject>();
	const FEWTool* Tool = nullptr;
	FEWResult Checked = Check(Name, Args, Tool);
	if (!Tool) { Done(MoveTemp(Checked)); return; }
	if (Tool->IsAsync()) { Tool->RunAsync(Args, MoveTemp(Done)); return; }
	Done(Tool->Run(Args));
}
