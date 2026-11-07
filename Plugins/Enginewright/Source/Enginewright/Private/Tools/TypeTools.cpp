#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EWTypes.h"
#include "EWProps.h"

#include "AssetToolsModule.h"
#include "Engine/UserDefinedEnum.h"
#include "Factories/EnumFactory.h"
#include "Factories/StructureFactory.h"
#include "IAssetTools.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/PackageName.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

namespace
{
	IAssetTools& AssetTools() { return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get(); }

	bool SaveNow(UObject* Asset, FEWResult& Res)
	{
		bool bDeferred = false;
		const bool bOk = EW::SaveOrDefer(Asset->GetOutermost(), bDeferred);
		if (!bOk) { Res.Warnings.Add(TEXT("Save failed; the asset is in memory only.")); }
		return bOk;
	}

	TSharedRef<FJsonObject> EnumInfo(const UEnum* E)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("path"), E->GetPathName());
		J->SetStringField(TEXT("kind"), TEXT("enum"));
		TArray<TSharedPtr<FJsonValue>> Values;
		for (int32 i = 0; i < E->NumEnums() - 1; ++i)
		{
			TSharedRef<FJsonObject> V = MakeShared<FJsonObject>();
			V->SetStringField(TEXT("name"), E->GetAuthoredNameStringByIndex(i));
			V->SetStringField(TEXT("internalName"), E->GetNameStringByIndex(i));
			V->SetNumberField(TEXT("value"), (double)E->GetValueByIndex(i));
			Values.Add(MakeShared<FJsonValueObject>(V));
		}
		J->SetArrayField(TEXT("values"), Values);
		return J;
	}

	TSharedRef<FJsonObject> StructInfo(UScriptStruct* S)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("path"), S->GetPathName());
		J->SetStringField(TEXT("kind"), TEXT("struct"));
		TArray<TSharedPtr<FJsonValue>> Fields;
		if (UUserDefinedStruct* U = Cast<UUserDefinedStruct>(S))
		{
			for (const FStructVariableDescription& D : FStructureEditorUtils::GetVarDesc(U))
			{
				TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
				F->SetStringField(TEXT("name"), D.FriendlyName);
				F->SetStringField(TEXT("type"), EW::PinTypeToString(D.ToPinType()));
				F->SetStringField(TEXT("default"), D.DefaultValue);
				Fields.Add(MakeShared<FJsonValueObject>(F));
			}
		}
		else
		{
			for (TFieldIterator<FProperty> It(S); It; ++It)
			{
				TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
				F->SetStringField(TEXT("name"), S->GetAuthoredNameForField(*It));
				F->SetStringField(TEXT("type"), It->GetCPPType());
				Fields.Add(MakeShared<FJsonValueObject>(F));
			}
		}
		J->SetArrayField(TEXT("fields"), Fields);
		return J;
	}

	bool CheckNewAsset(const FString& Path, FString& Folder, FString& Name, FEWResult& OutErr)
	{
		FString Err;
		if (!EW::SplitAssetPath(Path, Folder, Name, Err)) { OutErr = FEWResult::Error(TEXT("INVALID_PATH"), Err); return false; }
		if (EW::AssetPathTaken(Folder / Name))
		{
			OutErr = FEWResult::Error(TEXT("ASSET_EXISTS"), FString::Printf(TEXT("'%s' already exists. Pick another name or delete it first (asset_delete)."), *(Folder / Name)));
			return false;
		}
		return true;
	}
}

void RegisterTypeTools(FEWRegistry& R)
{
	R.Register(TEXT("enum_create"),
		TEXT("Create a Blueprint enum asset with the given value names, in order. Reads the enum back and checks every value."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string","description":"New asset path, e.g. /Game/Data/E_ItemCategory."},
			"values":{"type":"array","items":{"type":"string"},"description":"Value names in order (display names)."}},
			"required":["path","values"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const TArray<FString> Values = EW::StrList(Args, TEXT("values"));
			if (Values.Num() == 0) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("values must list at least one name.")); }
			TSet<FString> Seen;
			for (const FString& V : Values)
			{
				if (V.TrimStartAndEnd().IsEmpty()) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("An enum value name is empty.")); }
				bool bDup = false;
				Seen.Add(V.ToLower(), &bDup);
				if (bDup) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Value '%s' is listed twice."), *V)); }
			}
			FString Folder, Name;
			FEWResult Err;
			if (!CheckNewAsset(EW::Str(Args, TEXT("path")), Folder, Name, Err)) { return Err; }

			UUserDefinedEnum* E = Cast<UUserDefinedEnum>(AssetTools().CreateAsset(Name, Folder, UUserDefinedEnum::StaticClass(), NewObject<UEnumFactory>()));
			if (!E) { return FEWResult::Error(TEXT("CREATE_FAILED"), TEXT("The editor could not create the enum.")); }
			while (E->NumEnums() - 1 < Values.Num()) { FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(E); }
			for (int32 i = 0; i < Values.Num(); ++i)
			{
				if (!FEnumEditorUtils::SetEnumeratorDisplayName(E, i, FText::FromString(Values[i])))
				{
					return FEWResult::Error(TEXT("INVALID_VALUE"), FString::Printf(TEXT("'%s' is not a valid enum value name. The enum was created with default names; fix or delete it."), *Values[i]));
				}
			}
			FEWResult Res = FEWResult::Ok(EnumInfo(E));
			for (int32 i = 0; i < Values.Num(); ++i)
			{
				if (E->GetAuthoredNameStringByIndex(i) != Values[i])
				{
					return FEWResult::Error(TEXT("READBACK_MISMATCH"), FString::Printf(TEXT("Value %d reads back as '%s', not '%s'."), i, *E->GetAuthoredNameStringByIndex(i), *Values[i]));
				}
			}
			Res.Data->SetBoolField(TEXT("saved"), SaveNow(E, Res));
			return Res;
		}, /*bTransactional*/ false);

	R.Register(TEXT("struct_create"),
		TEXT("Create a Blueprint struct asset (usable as a DataTable row struct) with typed fields in order. Types: bool, int, int64, float, double, byte, string, name, text, vector, rotator, transform, color, object:<Class>, class:<Class>, softobject:<Class>, softclass:<Class>, struct:<Struct>, enum:<Enum>; add [] for an array."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string","description":"New asset path, e.g. /Game/Data/S_Item."},
			"fields":{"type":"array","items":{"type":"object"},"description":"[{\"name\":\"Weight\",\"type\":\"float\",\"default\":\"1.0\"}, ...]"}},
			"required":["path","fields"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
			Args->TryGetArrayField(TEXT("fields"), Fields);
			if (!Fields || Fields->Num() == 0) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("fields must list at least one field.")); }

			// Validate every field before creating anything.
			struct FField { FString Name; FEdGraphPinType Type; FString Default; };
			TArray<FField> Plan;
			TSet<FString> Seen;
			for (int32 i = 0; i < Fields->Num(); ++i)
			{
				const TSharedPtr<FJsonObject>* F = nullptr;
				if (!(*Fields)[i]->TryGetObject(F)) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("fields[%d] is not an object."), i)); }
				for (const auto& KV : (*F)->Values)
				{
					const FString K(*KV.Key);
					if (K != TEXT("name") && K != TEXT("type") && K != TEXT("default"))
					{
						return FEWResult::Error(TEXT("UNKNOWN_ARGUMENT"), FString::Printf(TEXT("fields[%d] has unknown key '%s' (allowed: name, type, default)."), i, *K),
							EW::Suggest(K, { TEXT("name"), TEXT("type"), TEXT("default") }));
					}
				}
				FField Out;
				(*F)->TryGetStringField(TEXT("name"), Out.Name);
				(*F)->TryGetStringField(TEXT("default"), Out.Default);
				FString TypeText, TypeErr;
				(*F)->TryGetStringField(TEXT("type"), TypeText);
				if (Out.Name.IsEmpty() || TypeText.IsEmpty()) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("fields[%d] needs name and type."), i)); }
				bool bDup = false;
				Seen.Add(Out.Name.ToLower(), &bDup);
				if (bDup) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Field '%s' is listed twice."), *Out.Name)); }
				if (!EW::ParsePinType(TypeText, Out.Type, TypeErr)) { return FEWResult::Error(TEXT("INVALID_TYPE"), FString::Printf(TEXT("fields[%d] (%s): %s"), i, *Out.Name, *TypeErr)); }
				Plan.Add(Out);
			}
			FString Folder, Name;
			FEWResult Err;
			if (!CheckNewAsset(EW::Str(Args, TEXT("path")), Folder, Name, Err)) { return Err; }

			UUserDefinedStruct* S = Cast<UUserDefinedStruct>(AssetTools().CreateAsset(Name, Folder, UUserDefinedStruct::StaticClass(), NewObject<UStructureFactory>()));
			if (!S) { return FEWResult::Error(TEXT("CREATE_FAILED"), TEXT("The editor could not create the struct.")); }
			TArray<FGuid> Initial;
			for (const FStructVariableDescription& D : FStructureEditorUtils::GetVarDesc(S)) { Initial.Add(D.VarGuid); }
			for (const FField& F : Plan)
			{
				FStructureEditorUtils::AddVariable(S, F.Type);
				const FGuid Guid = FStructureEditorUtils::GetVarDesc(S).Last().VarGuid;
				if (!FStructureEditorUtils::RenameVariable(S, Guid, F.Name))
				{
					return FEWResult::Error(TEXT("INVALID_VALUE"), FString::Printf(TEXT("'%s' is not a valid field name. The struct was created partly; delete it."), *F.Name));
				}
				if (!F.Default.IsEmpty() && !FStructureEditorUtils::ChangeVariableDefaultValue(S, Guid, F.Default))
				{
					return FEWResult::Error(TEXT("INVALID_VALUE"), FString::Printf(TEXT("Default '%s' does not fit field '%s'. The struct was created partly; delete it."), *F.Default, *F.Name));
				}
			}
			for (const FGuid& G : Initial) { FStructureEditorUtils::RemoveVariable(S, G); }

			// Read back: names and types in order.
			const TArray<FStructVariableDescription>& Desc = FStructureEditorUtils::GetVarDesc(S);
			if (Desc.Num() != Plan.Num()) { return FEWResult::Error(TEXT("READBACK_MISMATCH"), FString::Printf(TEXT("The struct has %d fields, expected %d."), Desc.Num(), Plan.Num())); }
			for (int32 i = 0; i < Plan.Num(); ++i)
			{
				if (Desc[i].FriendlyName != Plan[i].Name || EW::PinTypeToString(Desc[i].ToPinType()) != EW::PinTypeToString(Plan[i].Type))
				{
					return FEWResult::Error(TEXT("READBACK_MISMATCH"), FString::Printf(TEXT("Field %d reads back as %s (%s)."), i, *Desc[i].FriendlyName, *EW::PinTypeToString(Desc[i].ToPinType())));
				}
			}
			FEWResult Res = FEWResult::Ok(StructInfo(S));
			Res.Data->SetBoolField(TEXT("saved"), SaveNow(S, Res));
			return Res;
		}, /*bTransactional*/ false);

	R.Register(TEXT("type_info"),
		TEXT("Describe a struct (fields and types) or an enum (values), native or Blueprint. Use it to learn a DataTable row struct before writing rows."),
		TEXT(R"({"type":"object","properties":{
			"name":{"type":"string","description":"Short name (FItemRow, EMyEnum), /Script/Module.Name or a /Game asset path."}},
			"required":["name"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const FString Name = EW::Str(Args, TEXT("name"));
			FString ErrS, ErrE;
			if (UScriptStruct* S = EW::FindStruct(Name, ErrS)) { return FEWResult::Ok(StructInfo(S)); }
			if (UEnum* E = EW::FindEnum(Name, ErrE)) { return FEWResult::Ok(EnumInfo(E)); }
			return FEWResult::Error(TEXT("TYPE_NOT_FOUND"), ErrS + TEXT(" ") + ErrE);
		});
}
