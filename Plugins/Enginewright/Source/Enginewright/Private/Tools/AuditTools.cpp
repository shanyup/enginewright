#include "EnginewrightTool.h"
#include "EWCommon.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "K2Node_InputKey.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"

namespace
{
	FEWResult Bad(const FString& Code, const FString& Msg) { return FEWResult::Error(Code, Msg); }
	IAssetRegistry& Registry() { return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get(); }

	using FVisit = TFunctionRef<void(const FString& Where, const FString& Field, const FString& Value)>;

	/** Calls Visit for every text-like leaf value (name, string, text, object/soft reference path). */
	void Walk(const FProperty* P, const void* Value, const FString& Where, FVisit Visit, int32 Depth = 0)
	{
		if (!P || !Value || Depth > 8) { return; }
		const FString Field = P->GetAuthoredName();
		if (const FNameProperty* N = CastField<FNameProperty>(P)) { const FName V = N->GetPropertyValue(Value); if (!V.IsNone()) { Visit(Where, Field, V.ToString()); } return; }
		if (const FStrProperty* S = CastField<FStrProperty>(P)) { const FString& V = S->GetPropertyValue(Value); if (!V.IsEmpty()) { Visit(Where, Field, V); } return; }
		if (const FTextProperty* T = CastField<FTextProperty>(P)) { const FString V = T->GetPropertyValue(Value).ToString(); if (!V.IsEmpty()) { Visit(Where, Field, V); } return; }
		if (const FSoftObjectProperty* SO = CastField<FSoftObjectProperty>(P)) { const FString V = SO->GetPropertyValue(Value).ToSoftObjectPath().ToString(); if (!V.IsEmpty()) { Visit(Where, Field, V); } return; }
		if (const FObjectPropertyBase* O = CastField<FObjectPropertyBase>(P))
		{
			if (UObject* Obj = O->GetObjectPropertyValue(Value)) { if (Obj->IsAsset() || Obj->IsA<UClass>()) { Visit(Where, Field, Obj->GetPathName()); } }
			return;
		}
		if (const FStructProperty* SP = CastField<FStructProperty>(P))
		{
			for (TFieldIterator<FProperty> It(SP->Struct); It; ++It)
			{
				for (int32 i = 0; i < It->ArrayDim; ++i) { Walk(*It, It->ContainerPtrToValuePtr<void>(Value, i), Where + TEXT(".") + Field, Visit, Depth + 1); }
			}
			return;
		}
		if (const FArrayProperty* AP = CastField<FArrayProperty>(P))
		{
			FScriptArrayHelper H(AP, Value);
			for (int32 i = 0; i < H.Num(); ++i) { Walk(AP->Inner, H.GetRawPtr(i), FString::Printf(TEXT("%s.%s[%d]"), *Where, *Field, i), Visit, Depth + 1); }
			return;
		}
		if (const FMapProperty* MP = CastField<FMapProperty>(P))
		{
			FScriptMapHelper H(MP, Value);
			for (FScriptMapHelper::FIterator It(H); It; ++It)
			{
				Walk(MP->KeyProp, H.GetKeyPtr(It), Where + TEXT(".") + Field + TEXT("{key}"), Visit, Depth + 1);
				Walk(MP->ValueProp, H.GetValuePtr(It), Where + TEXT(".") + Field + TEXT("{value}"), Visit, Depth + 1);
			}
			return;
		}
		if (const FSetProperty* SetP = CastField<FSetProperty>(P))
		{
			FScriptSetHelper H(SetP, Value);
			for (FScriptSetHelper::FIterator It(H); It; ++It) { Walk(SetP->ElementProp, H.GetElementPtr(It), Where + TEXT(".") + Field, Visit, Depth + 1); }
		}
	}

	void WalkObject(const UStruct* Type, const void* Container, const FString& Where, FVisit Visit, bool bOnlyOwnBlueprintFields = false)
	{
		for (TFieldIterator<FProperty> It(Type); It; ++It)
		{
			if (bOnlyOwnBlueprintFields && !It->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)) { continue; }
			for (int32 i = 0; i < It->ArrayDim; ++i) { Walk(*It, It->ContainerPtrToValuePtr<void>(Container, i), Where, Visit); }
		}
	}

	/** Visits DataTable rows, Blueprint class defaults and data assets under Folder. Returns assets scanned. */
	int32 ScanContent(const FString& Folder, int32 MaxAssets, TFunctionRef<void(const FString& Asset, const FString& Where, const FString& Field, const FString& Value)> Visit)
	{
		FARFilter Filter;
		Filter.PackagePaths.Add(FName(*Folder));
		Filter.bRecursivePaths = true;
		Filter.ClassPaths.Add(UDataTable::StaticClass()->GetClassPathName());
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.ClassPaths.Add(UDataAsset::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		TArray<FAssetData> Assets;
		Registry().GetAssets(Filter, Assets);
		int32 Scanned = 0;
		for (const FAssetData& A : Assets)
		{
			if (Scanned >= MaxAssets) { break; }
			UObject* Obj = A.GetAsset();
			if (!Obj) { continue; }
			++Scanned;
			const FString Name = A.PackageName.ToString();
			auto V = [&](const FString& Where, const FString& Field, const FString& Value) { Visit(Name, Where, Field, Value); };
			if (UDataTable* DT = Cast<UDataTable>(Obj))
			{
				if (!DT->GetRowStruct()) { continue; }
				for (const TPair<FName, uint8*>& Row : DT->GetRowMap()) { WalkObject(DT->GetRowStruct(), Row.Value, TEXT("row ") + Row.Key.ToString(), V); }
			}
			else if (UBlueprint* BP = Cast<UBlueprint>(Obj))
			{
				if (BP->GeneratedClass && BP->GeneratedClass->GetDefaultObject())
				{
					// Only the Blueprint's own variables: inherited engine defaults are noise.
					for (TFieldIterator<FProperty> It(BP->GeneratedClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
					{
						Walk(*It, It->ContainerPtrToValuePtr<void>(BP->GeneratedClass->GetDefaultObject()), TEXT("defaults"), V);
					}
				}
			}
			else
			{
				WalkObject(Obj->GetClass(), Obj, TEXT("data"), V, true);
			}
		}
		return Scanned;
	}

	TSharedRef<FJsonObject> Hit(const FString& Asset, const FString& Where, const FString& Field, const FString& Value)
	{
		TSharedRef<FJsonObject> H = MakeShared<FJsonObject>();
		H->SetStringField(TEXT("asset"), Asset);
		H->SetStringField(TEXT("where"), Where);
		H->SetStringField(TEXT("field"), Field);
		H->SetStringField(TEXT("value"), Value);
		return H;
	}
}

int32 EW::ScanContentText(const FString& Folder, int32 MaxAssets, TFunctionRef<void(const FString&, const FString&, const FString&, const FString&)> Visit)
{
	return ScanContent(Folder, MaxAssets, Visit);
}

void RegisterAuditTools(FEWRegistry& R)
{
	R.Register(TEXT("asset_search_text"),
		TEXT("Find where a name, string, text or ID is stored inside content: DataTable rows, Blueprint variable defaults, data assets (no binary grep). Use it before renaming an ID or deleting an asset that is referenced by name."),
		TEXT(R"({"type":"object","properties":{
			"text":{"type":"string","description":"Name, string, text or ID to search for, e.g. Item_Sword."},
			"folder":{"type":"string","description":"Default /Game."},
			"exact":{"type":"boolean","description":"Whole value must match (case-insensitive). Default false = contains."},
			"maxAssets":{"type":"integer","description":"Assets to scan, default 5000."}},
			"required":["text"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const FString Text = EW::Str(Args, TEXT("text"));
			if (Text.TrimStartAndEnd().IsEmpty()) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("text is empty.")); }
			FString Folder = EW::Str(Args, TEXT("folder"));
			if (Folder.IsEmpty()) { Folder = TEXT("/Game"); }
			const bool bExact = EW::Bool(Args, TEXT("exact"), false);
			TArray<TSharedPtr<FJsonValue>> Hits;
			const int32 Scanned = ScanContent(Folder, (int32)EW::Num(Args, TEXT("maxAssets"), 5000), [&](const FString& A, const FString& W, const FString& F, const FString& V)
			{
				const bool bMatch = bExact ? V.Equals(Text, ESearchCase::IgnoreCase) : V.Contains(Text);
				if (bMatch && Hits.Num() < 500) { Hits.Add(MakeShared<FJsonValueObject>(Hit(A, W, F, V))); }
			});
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("assetsScanned"), Scanned);
			Data->SetArrayField(TEXT("hits"), Hits);
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("id_audit"),
		TEXT("Check that IDs stay consistent across content. Defined IDs come from a DataTable's row names (source) or a list (ids). Used IDs are the values of fields whose name matches fieldPattern (default: ends with ID) in DataTables, Blueprint defaults and data assets. Reports IDs used but never defined (broken) and defined but never used. ok is false (UNDEFINED_IDS) when an ID is used but not defined; see data.undefined."),
		TEXT(R"({"type":"object","properties":{
			"source":{"type":"string","description":"DataTable whose row names are the defined IDs."},
			"ids":{"type":"array","items":{"type":"string"},"description":"Defined IDs in addition to source row names, e.g. [\"ITEM_SWORD\"]."},
			"folder":{"type":"string","description":"Default /Game."},
			"fieldPattern":{"type":"string","description":"Regex for field names that hold IDs; default (?i).*ID$"}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			TSet<FString> Defined;
			const FString Source = EW::Str(Args, TEXT("source"));
			FString SourcePackage;
			if (!Source.IsEmpty())
			{
				FString Err;
				UDataTable* DT = EW::LoadAs<UDataTable>(Source, Err);
				if (!DT) { return Bad(TEXT("TABLE_NOT_FOUND"), Err); }
				SourcePackage = DT->GetOutermost()->GetName();
				for (const FName& N : DT->GetRowNames()) { Defined.Add(N.ToString()); }
			}
			for (const FString& S : EW::StrList(Args, TEXT("ids"))) { Defined.Add(S); }
			if (Defined.Num() == 0) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("Pass source (a DataTable) or ids.")); }
			FString Folder = EW::Str(Args, TEXT("folder"));
			if (Folder.IsEmpty()) { Folder = TEXT("/Game"); }
			FString PatternText = EW::Str(Args, TEXT("fieldPattern"));
			if (PatternText.IsEmpty()) { PatternText = TEXT("(?i).*ID$"); }
			const FRegexPattern Pattern(PatternText);

			TMap<FString, TArray<TSharedPtr<FJsonValue>>> Uses;
			int32 Fields = 0;
			const int32 Scanned = ScanContent(Folder, 5000, [&](const FString& A, const FString& W, const FString& F, const FString& V)
			{
				FRegexMatcher M(Pattern, F);
				if (!M.FindNext() || M.GetMatchBeginning() != 0 || M.GetMatchEnding() != F.Len()) { return; }
				if (V.StartsWith(TEXT("/"))) { return; } // References, not IDs.
				++Fields;
				TArray<TSharedPtr<FJsonValue>>& L = Uses.FindOrAdd(V);
				if (L.Num() < 20) { L.Add(MakeShared<FJsonValueObject>(Hit(A, W, F, V))); }
			});
			TArray<TSharedPtr<FJsonValue>> Undefined, Unused;
			for (const auto& U : Uses)
			{
				if (!Defined.Contains(U.Key))
				{
					TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
					J->SetStringField(TEXT("id"), U.Key);
					J->SetArrayField(TEXT("usedAt"), U.Value);
					TArray<FString> DefList = Defined.Array();
					const TArray<FString> Sug = EW::Suggest(U.Key, DefList);
					if (Sug.Num()) { J->SetStringField(TEXT("didYouMean"), Sug[0]); }
					Undefined.Add(MakeShared<FJsonValueObject>(J));
				}
			}
			for (const FString& D : Defined) { if (!Uses.Contains(D)) { Unused.Add(MakeShared<FJsonValueString>(D)); } }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("assetsScanned"), Scanned);
			Data->SetNumberField(TEXT("idFields"), Fields);
			Data->SetNumberField(TEXT("defined"), Defined.Num());
			Data->SetArrayField(TEXT("undefined"), Undefined);
			Data->SetArrayField(TEXT("unused"), Unused);
			FEWResult Res = Undefined.Num() ? FEWResult::Error(TEXT("UNDEFINED_IDS"), FString::Printf(TEXT("%d ID(s) are used but not defined; see data.undefined."), Undefined.Num())) : FEWResult::Ok();
			Res.Data = Data;
			return Res;
		});

	R.Register(TEXT("key_audit"),
		TEXT("Find hard-coded input keys: EKeys:: and FKey(\"...\") in the project's C++ source, and key input event nodes / key pin defaults in Blueprint graphs. Use it to enforce 'no hard-coded keys, use Enhanced Input'. ok is false (HARDCODED_KEYS) when any hard-coded key is found; the list is in data."),
		TEXT(R"({"type":"object","properties":{
			"folder":{"type":"string","description":"Content folder for Blueprints, default /Game."},
			"source":{"type":"boolean","description":"Scan the project's Source folder (default true)."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			TArray<TSharedPtr<FJsonValue>> Code, Graphs;
			if (EW::Bool(Args, TEXT("source"), true))
			{
				TArray<FString> Files;
				const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::GameSourceDir());
				IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*.cpp"), true, false);
				IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*.h"), true, false, false);
				const FRegexPattern KeyUse(TEXT("EKeys::[A-Za-z_0-9]+|FKey\\s*\\(\\s*(TEXT\\()?\"[^\"]+\""));
				for (const FString& F : Files)
				{
					TArray<FString> Lines;
					if (!FFileHelper::LoadFileToStringArray(Lines, *F)) { continue; }
					for (int32 i = 0; i < Lines.Num(); ++i)
					{
						if (Lines[i].TrimStart().StartsWith(TEXT("//"))) { continue; }
						FRegexMatcher M(KeyUse, Lines[i]);
						while (M.FindNext() && Code.Num() < 500)
						{
							TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
							J->SetStringField(TEXT("file"), F);
							J->SetNumberField(TEXT("line"), i + 1);
							J->SetStringField(TEXT("match"), M.GetCaptureGroup(0));
							Code.Add(MakeShared<FJsonValueObject>(J));
						}
					}
				}
			}
			FString Folder = EW::Str(Args, TEXT("folder"));
			if (Folder.IsEmpty()) { Folder = TEXT("/Game"); }
			int32 BPs = 0;
			for (const FString& P : EW::FindAssets(Folder, UBlueprint::StaticClass()))
			{
				FString Err;
				UBlueprint* BP = EW::LoadAs<UBlueprint>(P, Err);
				if (!BP) { continue; }
				++BPs;
				TArray<UEdGraph*> All;
				BP->GetAllGraphs(All);
				for (UEdGraph* G : All)
				{
					for (UEdGraphNode* N : G->Nodes)
					{
						if (UK2Node_InputKey* K = Cast<UK2Node_InputKey>(N))
						{
							TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
							J->SetStringField(TEXT("blueprint"), P);
							J->SetStringField(TEXT("graph"), G->GetName());
							J->SetStringField(TEXT("node"), N->GetNodeTitle(ENodeTitleType::ListView).ToString());
							J->SetStringField(TEXT("key"), K->InputKey.GetFName().ToString());
							Graphs.Add(MakeShared<FJsonValueObject>(J));
							continue;
						}
						for (UEdGraphPin* Pin : N ? N->Pins : TArray<UEdGraphPin*>())
						{
							if (Pin && Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() == 0 && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct &&
								Pin->PinType.PinSubCategoryObject == FKey::StaticStruct() && !Pin->DefaultValue.IsEmpty() && Pin->DefaultValue != TEXT("None"))
							{
								TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
								J->SetStringField(TEXT("blueprint"), P);
								J->SetStringField(TEXT("graph"), G->GetName());
								J->SetStringField(TEXT("node"), N->GetNodeTitle(ENodeTitleType::ListView).ToString());
								J->SetStringField(TEXT("key"), Pin->DefaultValue);
								Graphs.Add(MakeShared<FJsonValueObject>(J));
							}
						}
					}
				}
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("code"), Code);
			Data->SetArrayField(TEXT("blueprints"), Graphs);
			Data->SetNumberField(TEXT("blueprintsScanned"), BPs);
			const int32 Total = Code.Num() + Graphs.Num();
			FEWResult Res = Total ? FEWResult::Error(TEXT("HARDCODED_KEYS"), FString::Printf(TEXT("%d hard-coded key use(s) found."), Total)) : FEWResult::Ok();
			Res.Data = Data;
			return Res;
		});
}
