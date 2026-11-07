#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EWProps.h"

#include "AssetToolsModule.h"
#include "DataTableEditorUtils.h"
#include "Engine/DataTable.h"
#include "Factories/DataTableFactory.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

namespace
{
	UDataTable* LoadTable(const TSharedPtr<FJsonObject>& Args, FEWResult& OutErr)
	{
		FString Resolved, Err;
		UObject* Obj = EW::ResolveObject(EW::Str(Args, TEXT("path")), false, Resolved, Err);
		UDataTable* DT = Cast<UDataTable>(Obj);
		if (!DT)
		{
			OutErr = FEWResult::Error(TEXT("TABLE_NOT_FOUND"), Obj ? FString::Printf(TEXT("'%s' is a %s, not a DataTable."), *Resolved, *Obj->GetClass()->GetName()) : Err);
			return nullptr;
		}
		if (!DT->GetRowStruct())
		{
			OutErr = FEWResult::Error(TEXT("TABLE_HAS_NO_STRUCT"), TEXT("The table has no row struct (its C++ struct may be missing)."));
			return nullptr;
		}
		return DT;
	}

	TArray<FString> RowNames(const UDataTable* DT)
	{
		TArray<FString> Out;
		for (const FName& N : DT->GetRowNames()) { Out.Add(N.ToString()); }
		return Out;
	}

	const UScriptStruct* FindRowStruct(const FString& In)
	{
		if (In.StartsWith(TEXT("/")))
		{
			return FindObject<UScriptStruct>(nullptr, *In);
		}
		FString Name = In;
		if (Name.StartsWith(TEXT("F")) && !FindFirstObject<UScriptStruct>(*Name, EFindFirstObjectOptions::None)) { Name.RightChopInline(1); }
		return FindFirstObject<UScriptStruct>(*Name, EFindFirstObjectOptions::None);
	}

	FEWResult SaveTable(UDataTable* DT, bool bSave, FEWResult Res)
	{
		DT->MarkPackageDirty();
		bool bDeferred = false, bSaved = false;
		if (bSave)
		{
			bSaved = EW::SaveOrDefer(DT->GetOutermost(), bDeferred);
			if (!bSaved) { Res.Warnings.Add(TEXT("Save failed; the change is in memory only.")); }
		}
		if (!Res.Data.IsValid()) { Res.Data = MakeShared<FJsonObject>(); }
		Res.Data->SetBoolField(TEXT("saved"), bSaved && !bDeferred);
		return Res;
	}
}

void RegisterDataTableTools(FEWRegistry& R)
{
	R.Register(TEXT("datatable_create"),
		TEXT("Create a DataTable for a row struct (C++ struct derived from FTableRowBase, or a path /Script/Module.Struct)."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string","description":"New asset path, e.g. /Game/Data/DT_Items."},
			"rowStruct":{"type":"string","description":"ItemDefinition, FItemDefinition or /Script/MyGame.ItemDefinition."}},
			"required":["path","rowStruct"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const FString Path = EW::Str(Args, TEXT("path"));
			const UScriptStruct* RS = FindRowStruct(EW::Str(Args, TEXT("rowStruct")));
			if (!RS || !RS->IsChildOf(FTableRowBase::StaticStruct()))
			{
				return FEWResult::Error(TEXT("UNKNOWN_ROW_STRUCT"), FString::Printf(TEXT("'%s' is not a struct derived from FTableRowBase."), *EW::Str(Args, TEXT("rowStruct"))));
			}
			if (FPackageName::DoesPackageExist(Path)) { return FEWResult::Error(TEXT("ASSET_EXISTS"), FString::Printf(TEXT("'%s' already exists."), *Path)); }
			UDataTableFactory* Factory = NewObject<UDataTableFactory>();
			Factory->Struct = const_cast<UScriptStruct*>(RS);
			IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
			UObject* Asset = AT.CreateAsset(FPackageName::GetShortName(Path), FPackageName::GetLongPackagePath(Path), UDataTable::StaticClass(), Factory);
			UDataTable* DT = Cast<UDataTable>(Asset);
			if (!DT) { return FEWResult::Error(TEXT("CREATE_FAILED"), TEXT("The engine could not create the table (check the folder path).")); }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("path"), DT->GetOutermost()->GetName());
			Data->SetStringField(TEXT("rowStruct"), RS->GetPathName());
			return SaveTable(DT, true, FEWResult::Ok(Data));
		}, /*bTransactional*/ false);

	R.Register(TEXT("datatable_get"),
		TEXT("Read a DataTable: its row struct, the row names, and the values of the rows asked for (or all rows when rows is omitted)."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string"},
			"rows":{"type":"array","items":{"type":"string"},"description":"Row names to read. Omit for all."},
			"fields":{"type":"array","items":{"type":"string"},"description":"Field names to read. Omit for all."},
			"namesOnly":{"type":"boolean","description":"Return only the row names and the field list."}},
			"required":["path"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UDataTable* DT = LoadTable(Args, Err);
			if (!DT) { return Err; }
			const UScriptStruct* RS = DT->GetRowStruct();

			TArray<FString> Fields, Wanted;
			const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
			if (Args->TryGetArrayField(TEXT("fields"), List)) { for (const auto& V : *List) { Fields.Add(V->AsString()); } }
			if (Args->TryGetArrayField(TEXT("rows"), List)) { for (const auto& V : *List) { Wanted.Add(V->AsString()); } }
			bool bNamesOnly = false;
			Args->TryGetBoolField(TEXT("namesOnly"), bNamesOnly);

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("rowStruct"), RS->GetPathName());
			TArray<TSharedPtr<FJsonValue>> Names, FieldNames;
			for (const FString& N : RowNames(DT)) { Names.Add(MakeShared<FJsonValueString>(N)); }
			for (const FString& F : EW::PropertyNames(RS)) { FieldNames.Add(MakeShared<FJsonValueString>(F)); }
			Data->SetArrayField(TEXT("rowNames"), Names);
			Data->SetArrayField(TEXT("fields"), FieldNames);
			if (bNamesOnly) { return FEWResult::Ok(Data); }

			if (Wanted.Num() == 0) { Wanted = RowNames(DT); }
			TSharedRef<FJsonObject> Rows = MakeShared<FJsonObject>();
			for (const FString& Row : Wanted)
			{
				const uint8* Mem = DT->FindRowUnchecked(FName(*Row));
				if (!Mem)
				{
					return FEWResult::Error(TEXT("ROW_NOT_FOUND"), FString::Printf(TEXT("No row '%s'."), *Row), EW::Suggest(Row, RowNames(DT)));
				}
				TSharedPtr<FJsonObject> Values;
				FEWResult R2 = EW::ReadValues(RS, Mem, Fields, Values);
				if (!R2.bOk) { return R2; }
				Rows->SetObjectField(Row, Values);
			}
			Data->SetObjectField(TEXT("rows"), Rows);
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("datatable_set_rows"),
		TEXT("Add or update DataTable rows in one step: {rowName: {field: value}}. Fields are checked like object_set (unknown field, type, enum = error). Partial rows keep their other fields. If any row fails, the whole table is restored."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string"},
			"rows":{"type":"object","description":"Row name -> object of field values."},
			"mode":{"type":"string","enum":["upsert","add","update"],"description":"upsert (default): add missing rows, update existing; add: fail if a row exists; update: fail if a row is missing."},
			"save":{"type":"boolean","description":"Save afterwards (default true)."}},
			"required":["path","rows"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UDataTable* DT = LoadTable(Args, Err);
			if (!DT) { return Err; }
			const UScriptStruct* RS = DT->GetRowStruct();
			const TSharedPtr<FJsonObject>* Rows = nullptr;
			if (!Args->TryGetObjectField(TEXT("rows"), Rows) || (*Rows)->Values.Num() == 0)
			{
				return FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("rows must be a non-empty object."));
			}
			FString Mode = EW::Str(Args, TEXT("mode"));
			if (Mode.IsEmpty()) { Mode = TEXT("upsert"); }
			bool bSave = true;
			Args->TryGetBoolField(TEXT("save"), bSave);

			for (const auto& Pair : (*Rows)->Values)
			{
				const FString Row(*Pair.Key);
				const bool bExists = DT->FindRowUnchecked(FName(*Row)) != nullptr;
				if (Mode == TEXT("add") && bExists) { return FEWResult::Error(TEXT("ROW_EXISTS"), FString::Printf(TEXT("Row '%s' already exists (mode add). Nothing was written."), *Row)); }
				if (Mode == TEXT("update") && !bExists)
				{
					return FEWResult::Error(TEXT("ROW_NOT_FOUND"), FString::Printf(TEXT("No row '%s' (mode update). Nothing was written."), *Row), EW::Suggest(Row, RowNames(DT)));
				}
				if (Pair.Value->Type != EJson::Object) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Row '%s' must be an object of fields."), *Row)); }
			}

			const FString Snapshot = DT->GetTableAsJSON();
			FScopedTransaction Tx(FText::FromString(TEXT("Enginewright datatable_set_rows")));
			DT->Modify();
			FDataTableEditorUtils::BroadcastPreChange(DT, FDataTableEditorUtils::EDataTableChangeInfo::RowList);

			FEWResult Res = FEWResult::Ok();
			TSharedRef<FJsonObject> AllChanges = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Added;
			for (const auto& Pair : (*Rows)->Values)
			{
				const FName Row(*Pair.Key);
				uint8* Mem = DT->FindRowUnchecked(Row);
				if (!Mem)
				{
					Mem = FDataTableEditorUtils::AddRow(DT, Row);
					Added.Add(MakeShared<FJsonValueString>(Row.ToString()));
				}
				FEWResult One = Mem ? EW::WriteValues(RS, Mem, nullptr, Pair.Value->AsObject()) : FEWResult::Error(TEXT("ADD_ROW_FAILED"), TEXT("Could not add the row."));
				if (!One.bOk)
				{
					DT->CreateTableFromJSONString(Snapshot);
					FDataTableEditorUtils::BroadcastPostChange(DT, FDataTableEditorUtils::EDataTableChangeInfo::RowList);
					EW::CancelUnlessBatch(Tx);
					One.Message = FString::Printf(TEXT("Row '%s': %s The table was restored."), *Row.ToString(), *One.Message);
					return One;
				}
				Res.Warnings.Append(One.Warnings);
				AllChanges->SetObjectField(Row.ToString(), One.Data->GetObjectField(TEXT("changes")));
			}
			FDataTableEditorUtils::BroadcastPostChange(DT, FDataTableEditorUtils::EDataTableChangeInfo::RowList);

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetObjectField(TEXT("changes"), AllChanges);
			Data->SetArrayField(TEXT("addedRows"), Added);
			Res.Data = Data;
			return SaveTable(DT, bSave, Res);
		});

	R.Register(TEXT("datatable_remove_rows"),
		TEXT("Remove DataTable rows. All names must exist, otherwise nothing is removed."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string"},
			"rows":{"type":"array","items":{"type":"string"}},
			"save":{"type":"boolean"}},
			"required":["path","rows"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UDataTable* DT = LoadTable(Args, Err);
			if (!DT) { return Err; }
			const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
			Args->TryGetArrayField(TEXT("rows"), List);
			for (const auto& V : *List)
			{
				if (!DT->FindRowUnchecked(FName(*V->AsString())))
				{
					return FEWResult::Error(TEXT("ROW_NOT_FOUND"), FString::Printf(TEXT("No row '%s'. Nothing was removed."), *V->AsString()), EW::Suggest(V->AsString(), RowNames(DT)));
				}
			}
			FScopedTransaction Tx(FText::FromString(TEXT("Enginewright datatable_remove_rows")));
			for (const auto& V : *List) { FDataTableEditorUtils::RemoveRow(DT, FName(*V->AsString())); }
			bool bSave = true;
			Args->TryGetBoolField(TEXT("save"), bSave);
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("removed"), List->Num());
			return SaveTable(DT, bSave, FEWResult::Ok(Data));
		});

	R.Register(TEXT("datatable_rename_row"),
		TEXT("Rename one DataTable row. Note: other assets that store the old name as an ID are not changed; search them with asset text search first."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string"},"from":{"type":"string"},"to":{"type":"string"},"save":{"type":"boolean"}},
			"required":["path","from","to"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UDataTable* DT = LoadTable(Args, Err);
			if (!DT) { return Err; }
			const FString From = EW::Str(Args, TEXT("from")), To = EW::Str(Args, TEXT("to"));
			if (!DT->FindRowUnchecked(FName(*From))) { return FEWResult::Error(TEXT("ROW_NOT_FOUND"), FString::Printf(TEXT("No row '%s'."), *From), EW::Suggest(From, RowNames(DT))); }
			if (DT->FindRowUnchecked(FName(*To))) { return FEWResult::Error(TEXT("ROW_EXISTS"), FString::Printf(TEXT("Row '%s' already exists."), *To)); }
			FScopedTransaction Tx(FText::FromString(TEXT("Enginewright datatable_rename_row")));
			if (!FDataTableEditorUtils::RenameRow(DT, FName(*From), FName(*To))) { EW::CancelUnlessBatch(Tx); return FEWResult::Error(TEXT("RENAME_FAILED"), TEXT("The engine refused the rename.")); }
			bool bSave = true;
			Args->TryGetBoolField(TEXT("save"), bSave);
			return SaveTable(DT, bSave, FEWResult::Ok());
		});
}
