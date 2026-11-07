#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EWProps.h"

#include "ScopedTransaction.h"
#include "UObject/Package.h"

void RegisterObjectTools(FEWRegistry& R)
{
	R.Register(TEXT("object_get"),
		TEXT("Read properties of any object: an asset, a Blueprint's defaults (classDefault true), a placed actor or a component (Path:Component). Omit properties to read all editable ones."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string","description":"Object path: /Game/X, /Game/X.X, /Game/BP_X.Default__BP_X_C:Mesh, or a level actor path."},
			"properties":{"type":"array","items":{"type":"string"},"description":"Property names to read (case-insensitive). Omit for all."},
			"classDefault":{"type":"boolean","description":"For a Blueprint or class path, read its class default object."}},
			"required":["path"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Resolved, Err;
			bool bCDO = false;
			Args->TryGetBoolField(TEXT("classDefault"), bCDO);
			UObject* Obj = EW::ResolveObject(EW::Str(Args, TEXT("path")), bCDO, Resolved, Err);
			if (!Obj) { return FEWResult::Error(TEXT("OBJECT_NOT_FOUND"), Err); }

			TArray<FString> Wanted;
			const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
			if (Args->TryGetArrayField(TEXT("properties"), List))
			{
				for (const TSharedPtr<FJsonValue>& V : *List) { Wanted.Add(V->AsString()); }
			}
			TSharedPtr<FJsonObject> Props;
			FEWResult Res = EW::ReadValues(Obj->GetClass(), Obj, Wanted, Props);
			if (!Res.bOk) { return Res; }

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("path"), Resolved);
			Data->SetStringField(TEXT("class"), Obj->GetClass()->GetPathName());
			Data->SetObjectField(TEXT("properties"), Props);
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("object_set"),
		TEXT("Write properties on any object, checked and verified: unknown property or struct field, wrong type or enum name = error with suggestions; all values are validated before anything is written (all or nothing, one undo step); every value is read back and compared. References take a path string; {refPath} is also accepted."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string","description":"Object path, as for object_get."},
			"values":{"type":"object","description":"Property name -> new value. Structs may be partial; arrays replace the whole array."},
			"classDefault":{"type":"boolean","description":"For a Blueprint or class path, write its class default object."},
			"save":{"type":"boolean","description":"Save the package afterwards (default true)."}},
			"required":["path","values"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Resolved, Err;
			bool bCDO = false, bSave = true;
			Args->TryGetBoolField(TEXT("classDefault"), bCDO);
			Args->TryGetBoolField(TEXT("save"), bSave);
			UObject* Obj = EW::ResolveObject(EW::Str(Args, TEXT("path")), bCDO, Resolved, Err);
			if (!Obj) { return FEWResult::Error(TEXT("OBJECT_NOT_FOUND"), Err); }

			const TSharedPtr<FJsonObject>* Values = nullptr;
			Args->TryGetObjectField(TEXT("values"), Values);

			FScopedTransaction Tx(FText::FromString(TEXT("Enginewright object_set")));
			FEWResult Res = EW::WriteValues(Obj->GetClass(), Obj, Obj, Values ? *Values : nullptr);
			if (!Res.bOk)
			{
				EW::CancelUnlessBatch(Tx);
				return Res;
			}

			Obj->MarkPackageDirty();
			bool bSaved = false, bDeferred = false;
			if (bSave)
			{
				bSaved = EW::SaveOrDefer(Obj->GetOutermost(), bDeferred);
				if (!bSaved) { Res.Warnings.Add(TEXT("Save failed; the change is in memory only.")); }
			}
			Res.Data->SetStringField(TEXT("path"), Resolved);
			Res.Data->SetBoolField(TEXT("saved"), bSaved && !bDeferred);
			if (bDeferred) { Res.Data->SetBoolField(TEXT("saveDeferredToBatchEnd"), true); }
			Res.Data->SetBoolField(TEXT("undoable"), true);
			return Res;
		});
}
