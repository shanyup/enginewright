#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EWProps.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "FileHelpers.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"

namespace
{
	IAssetRegistry& Registry() { return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get(); }
	IAssetTools& Tools() { return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get(); }

	/** /Game/A/B.B -> /Game/A/B */
	FString PackageOf(const FString& Path)
	{
		FString P = Path.TrimStartAndEnd();
		int32 Dot;
		if (P.FindChar(TEXT('.'), Dot)) { P = P.Left(Dot); }
		return P;
	}

	bool AssetExists(const FString& Package)
	{
		TArray<FAssetData> Found;
		Registry().GetAssetsByPackageName(FName(*Package), Found, /*bIncludeOnlyOnDiskAssets*/ false);
		return Found.Num() > 0;
	}

	UObject* LoadAsset(const FString& Path, FString& OutErr)
	{
		const FString Package = PackageOf(Path);
		if (!AssetExists(Package))
		{
			OutErr = FString::Printf(TEXT("No asset at '%s'."), *Package);
			return nullptr;
		}
		FString Resolved;
		return EW::ResolveObject(Package, false, Resolved, OutErr);
	}

	TArray<FString> Referencers(const FString& Package)
	{
		TArray<FName> Out;
		Registry().GetReferencers(FName(*Package), Out, UE::AssetRegistry::EDependencyCategory::Package);
		TArray<FString> S;
		for (const FName& N : Out) { if (N.ToString() != Package) { S.Add(N.ToString()); } }
		return S;
	}

	TArray<TSharedPtr<FJsonValue>> ToJson(const TArray<FString>& In)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& S : In) { Out.Add(MakeShared<FJsonValueString>(S)); }
		return Out;
	}

	bool ResolveClassPath(const FString& In, FTopLevelAssetPath& Out)
	{
		if (In.StartsWith(TEXT("/")))
		{
			Out = FTopLevelAssetPath(In);
			return Out.IsValid();
		}
		if (UClass* C = FindFirstObject<UClass>(*In, EFindFirstObjectOptions::None))
		{
			Out = C->GetClassPathName();
			return true;
		}
		return false;
	}
}

void RegisterAssetTools(FEWRegistry& R)
{
	R.Register(TEXT("asset_find"),
		TEXT("Find assets in the Content Browser by folder, name wildcard and class. Works during Play In Editor."),
		TEXT(R"({"type":"object","properties":{
			"folder":{"type":"string","description":"Folder to search (default /Game)."},
			"name":{"type":"string","description":"Asset name wildcard, e.g. BP_* or *Door*."},
			"class":{"type":"string","description":"Class short name (DataTable, Blueprint, Material) or path (/Script/Engine.DataTable)."},
			"recursive":{"type":"boolean","description":"Include sub-folders (default true)."},
			"limit":{"type":"integer","description":"Max results (default 50)."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FARFilter Filter;
			const FString Folder = EW::Str(Args, TEXT("folder"));
			Filter.PackagePaths.Add(FName(*(Folder.IsEmpty() ? FString(TEXT("/Game")) : Folder)));
			bool bRecursive = true;
			Args->TryGetBoolField(TEXT("recursive"), bRecursive);
			Filter.bRecursivePaths = bRecursive;
			const FString Class = EW::Str(Args, TEXT("class"));
			if (!Class.IsEmpty())
			{
				FTopLevelAssetPath CP;
				if (!ResolveClassPath(Class, CP)) { return FEWResult::Error(TEXT("UNKNOWN_CLASS"), FString::Printf(TEXT("No class named '%s'."), *Class)); }
				Filter.ClassPaths.Add(CP);
				Filter.bRecursiveClasses = true;
			}
			TArray<FAssetData> Found;
			Registry().GetAssets(Filter, Found);
			const FString Name = EW::Str(Args, TEXT("name"));
			int32 Limit = 50;
			Args->TryGetNumberField(TEXT("limit"), Limit);

			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 Total = 0;
			for (const FAssetData& A : Found)
			{
				if (!Name.IsEmpty() && !A.AssetName.ToString().MatchesWildcard(Name)) { continue; }
				++Total;
				if (Rows.Num() >= Limit) { continue; }
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("path"), A.PackageName.ToString());
				Row->SetStringField(TEXT("class"), A.AssetClassPath.GetAssetName().ToString());
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("assets"), Rows);
			Data->SetNumberField(TEXT("total"), Total);
			FEWResult Res = FEWResult::Ok(Data);
			if (Total > Rows.Num()) { Res.Warnings.Add(FString::Printf(TEXT("%d more not shown; raise limit or narrow the search."), Total - Rows.Num())); }
			return Res;
		});

	R.Register(TEXT("asset_exists"),
		TEXT("Check whether an asset exists. Correct during Play In Editor too."),
		TEXT(R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("exists"), AssetExists(PackageOf(EW::Str(Args, TEXT("path")))));
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("asset_references"),
		TEXT("List what uses an asset (referencers) or what it uses (dependencies)."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string"},
			"direction":{"type":"string","enum":["referencers","dependencies"],"description":"Default referencers."}},
			"required":["path"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const FString Package = PackageOf(EW::Str(Args, TEXT("path")));
			if (!AssetExists(Package)) { return FEWResult::Error(TEXT("ASSET_NOT_FOUND"), FString::Printf(TEXT("No asset at '%s'."), *Package)); }
			TArray<FString> List;
			if (EW::Str(Args, TEXT("direction")) == TEXT("dependencies"))
			{
				TArray<FName> Out;
				Registry().GetDependencies(FName(*Package), Out, UE::AssetRegistry::EDependencyCategory::Package);
				for (const FName& N : Out) { List.Add(N.ToString()); }
			}
			else
			{
				List = Referencers(Package);
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("packages"), ToJson(List));
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("asset_duplicate"),
		TEXT("Copy an asset to a new path and save the copy."),
		TEXT(R"({"type":"object","properties":{
			"source":{"type":"string"},
			"destination":{"type":"string","description":"Full new path, e.g. /Game/Items/BP_Apple2."}},
			"required":["source","destination"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Err;
			UObject* Src = LoadAsset(EW::Str(Args, TEXT("source")), Err);
			if (!Src) { return FEWResult::Error(TEXT("ASSET_NOT_FOUND"), Err); }
			const FString Dest = PackageOf(EW::Str(Args, TEXT("destination")));
			if (AssetExists(Dest)) { return FEWResult::Error(TEXT("ASSET_EXISTS"), FString::Printf(TEXT("'%s' already exists."), *Dest)); }
			UObject* Copy = Tools().DuplicateAsset(FPackageName::GetShortName(Dest), FPackageName::GetLongPackagePath(Dest), Src);
			if (!Copy) { return FEWResult::Error(TEXT("DUPLICATE_FAILED"), TEXT("The engine refused the copy (check the destination folder name).")); }
			bool bDeferred = false;
			const bool bSaved = EW::SaveOrDefer(Copy->GetOutermost(), bDeferred);
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("path"), Copy->GetOutermost()->GetName());
			Data->SetBoolField(TEXT("saved"), bSaved && !bDeferred);
			return FEWResult::Ok(Data);
		}, /*bTransactional*/ false);

	R.Register(TEXT("asset_rename"),
		TEXT("Rename or move an asset. Every referencer is updated and the redirector left behind is fixed up and removed."),
		TEXT(R"({"type":"object","properties":{
			"source":{"type":"string"},
			"destination":{"type":"string","description":"Full new path, e.g. /Game/Items/BP_Apple."}},
			"required":["source","destination"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Err;
			const FString Old = PackageOf(EW::Str(Args, TEXT("source")));
			UObject* Obj = LoadAsset(Old, Err);
			if (!Obj) { return FEWResult::Error(TEXT("ASSET_NOT_FOUND"), Err); }
			const FString Dest = PackageOf(EW::Str(Args, TEXT("destination")));
			if (AssetExists(Dest)) { return FEWResult::Error(TEXT("ASSET_EXISTS"), FString::Printf(TEXT("'%s' already exists."), *Dest)); }
			const FString OldObjectPath = Obj->GetPathName();

			TArray<FAssetRenameData> Renames;
			Renames.Emplace(Obj, FPackageName::GetLongPackagePath(Dest), FPackageName::GetShortName(Dest));
			if (!Tools().RenameAssets(Renames))
			{
				return FEWResult::Error(TEXT("RENAME_FAILED"), TEXT("The engine refused the rename."));
			}

			FEWResult Res = FEWResult::Ok();
			if (UObjectRedirector* Redirector = FindObject<UObjectRedirector>(nullptr, *OldObjectPath))
			{
				Tools().FixupReferencers({ Redirector }, /*bCheckoutDialogPrompt*/ false);
			}
			if (AssetExists(Old)) { Res.Warnings.Add(TEXT("A redirector is still at the old path; something could not be fixed up.")); }

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("path"), Dest);
			Data->SetBoolField(TEXT("exists"), AssetExists(Dest));
			Data->SetArrayField(TEXT("referencersUpdated"), ToJson(Referencers(Dest)));
			Res.Data = Data;
			return Res;
		}, /*bTransactional*/ false);

	R.Register(TEXT("asset_delete"),
		TEXT("Delete assets. Refused while anything outside the deleted set still references them (the reply lists who); force deletes anyway and breaks those references."),
		TEXT(R"({"type":"object","properties":{
			"paths":{"type":"array","items":{"type":"string"}},
			"force":{"type":"boolean","description":"Delete even when referenced (default false)."}},
			"required":["paths"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr;
			Args->TryGetArrayField(TEXT("paths"), Paths);
			bool bForce = false;
			Args->TryGetBoolField(TEXT("force"), bForce);

			TSet<FString> Set;
			TArray<UObject*> Objects;
			for (const TSharedPtr<FJsonValue>& V : *Paths)
			{
				FString Err;
				UObject* Obj = LoadAsset(V->AsString(), Err);
				if (!Obj) { return FEWResult::Error(TEXT("ASSET_NOT_FOUND"), Err + TEXT(" Nothing was deleted.")); }
				Objects.Add(Obj);
				Set.Add(Obj->GetOutermost()->GetName());
			}

			TArray<FString> Blocking;
			for (const FString& Package : Set)
			{
				for (const FString& Ref : Referencers(Package))
				{
					if (!Set.Contains(Ref)) { Blocking.Add(FString::Printf(TEXT("%s (used by %s)"), *Package, *Ref)); }
				}
			}
			if (Blocking.Num() && !bForce)
			{
				FEWResult Res = FEWResult::Error(TEXT("ASSET_REFERENCED"),
					TEXT("Nothing was deleted: other assets still use these. Fix the referencers first, or pass force true."));
				TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetArrayField(TEXT("blocking"), ToJson(Blocking));
				Res.Data = Data;
				return Res;
			}

			const int32 Deleted = bForce ? ObjectTools::ForceDeleteObjects(Objects, false) : ObjectTools::DeleteObjects(Objects, false);
			TArray<FString> Left;
			for (const FString& Package : Set) { if (AssetExists(Package)) { Left.Add(Package); } }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("deleted"), Deleted);
			Data->SetArrayField(TEXT("stillThere"), ToJson(Left));
			if (Left.Num())
			{
				FEWResult Res = FEWResult::Error(TEXT("DELETE_INCOMPLETE"), TEXT("Some assets are still there (open in an editor, or in use)."));
				Res.Data = Data;
				return Res;
			}
			FEWResult Res = FEWResult::Ok(Data);
			if (Blocking.Num()) { Res.Warnings.Add(FString::Printf(TEXT("Forced: %d reference(s) are now broken."), Blocking.Num())); }
			return Res;
		}, /*bTransactional*/ false);

	R.Register(TEXT("asset_save"),
		TEXT("Save assets by path, or every unsaved asset and level when paths is empty."),
		TEXT(R"({"type":"object","properties":{"paths":{"type":"array","items":{"type":"string"}}}})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr;
			TArray<UPackage*> Packages;
			if (Args->TryGetArrayField(TEXT("paths"), Paths) && Paths->Num())
			{
				for (const TSharedPtr<FJsonValue>& V : *Paths)
				{
					FString Err;
					UObject* Obj = LoadAsset(V->AsString(), Err);
					if (!Obj) { return FEWResult::Error(TEXT("ASSET_NOT_FOUND"), Err); }
					Packages.Add(Obj->GetOutermost());
				}
			}
			else
			{
				FEditorFileUtils::GetDirtyContentPackages(Packages);
				FEditorFileUtils::GetDirtyWorldPackages(Packages);
			}
			const bool bOk = Packages.Num() == 0 || UEditorLoadingAndSavingUtils::SavePackages(Packages, false);
			TArray<FString> Names;
			for (UPackage* P : Packages) { Names.Add(P->GetName()); }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("saved"), ToJson(Names));
			return bOk ? FEWResult::Ok(Data) : FEWResult::Error(TEXT("SAVE_FAILED"), TEXT("One or more packages could not be saved (read-only file or source control?)."));
		}, /*bTransactional*/ false);
}
