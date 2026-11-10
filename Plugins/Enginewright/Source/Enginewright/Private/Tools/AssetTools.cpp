#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EWProps.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UObjectGlobals.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "IAssetTools.h"
#include "Internationalization/StringTable.h"
#include "Misc/PackageName.h"
#include "Misc/RedirectCollector.h"
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
		// The registry is incomplete while the editor still scans content after start-up: also ask the disk.
		return Found.Num() > 0 || EW::AssetPathTaken(Package);
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

	/** Orders the packages so that referencers come before what they reference; the given order on a cycle. */
	TArray<FString> ReferencersFirst(const TSet<FString>& Set, const TArray<UObject*>& Objects)
	{
		TArray<FString> Nodes = Set.Array();
		Nodes.Sort();
		TMap<FString, TArray<FString>> Deps;
		TMap<FString, int32> Indeg;
		for (const FString& P : Nodes) { Deps.Add(P, TArray<FString>()); Indeg.Add(P, 0); }
		for (const FString& P : Nodes)
		{
			TArray<FName> Out;
			Registry().GetDependencies(FName(*P), Out, UE::AssetRegistry::EDependencyCategory::Package);
			for (const FName& D : Out)
			{
				const FString DS = D.ToString();
				if (DS != P && Set.Contains(DS)) { Deps[P].AddUnique(DS); }
			}
		}
		for (const FString& P : Nodes) { for (const FString& D : Deps[P]) { Indeg[D] += 1; } }
		TArray<FString> Ready, Order;
		for (const FString& P : Nodes) { if (Indeg[P] == 0) { Ready.Add(P); } }
		while (Ready.Num())
		{
			const FString P = Ready[0];
			Ready.RemoveAt(0);
			Order.Add(P);
			for (const FString& D : Deps[P]) { if (--Indeg[D] == 0) { Ready.Add(D); } }
		}
		if (Order.Num() != Nodes.Num())
		{
			Order.Reset();
			for (const UObject* Obj : Objects) { Order.AddUnique(Obj->GetOutermost()->GetName()); }
		}
		return Order;
	}

	TArray<TSharedPtr<FJsonValue>> ToJson(const TArray<FString>& In)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& S : In) { Out.Add(MakeShared<FJsonValueString>(S)); }
		return Out;
	}

	/** Class name of the asset in a package ("Blueprint", "Material"), from the registry. */
	FString AssetClassOf(const FString& Package)
	{
		TArray<FAssetData> Found;
		Registry().GetAssetsByPackageName(FName(*Package), Found, false);
		return Found.Num() ? Found[0].AssetClassPath.GetAssetName().ToString() : FString();
	}

	/**
	 * Why an asset cannot be (or was not) deleted: assets and packages that reference it, an open editor, unsaved changes,
	 * a read-only file and (bMemory) who holds the loaded object in memory, which is what keeps it from being freed.
	 */
	TArray<FString> WhyAssetStays(const FString& Package, UObject* Obj, const TSet<FString>& DeleteSet, bool bMemory)
	{
		TArray<FString> Why;
		TArray<FString> Users;
		for (const FString& Ref : Referencers(Package))
		{
			if (DeleteSet.Contains(Ref)) { continue; }
			const FString Class = AssetClassOf(Ref);
			Users.Add(Class.IsEmpty() ? Ref : FString::Printf(TEXT("%s (%s)"), *Ref, *Class));
		}
		if (Users.Num()) { Why.Add(FString::Printf(TEXT("referenced by %s"), *FString::Join(Users, TEXT(", ")))); }
		if (Obj && IsValid(Obj))
		{
			if (GEditor)
			{
				if (UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
				{
					if (Editors->FindEditorsForAssetAndSubObjects(Obj).Num()) { Why.Add(TEXT("open in an asset editor: close it first")); }
				}
			}
			if (Obj->GetOutermost()->IsDirty()) { Why.Add(TEXT("the package has unsaved changes")); }
			if (bMemory)
			{
				// Who holds the loaded object in memory (what the delete reports as "in use"): objects outside the asset that point to it.
				if (Obj->IsRooted()) { Why.Add(TEXT("the object is rooted (added to the root set by C++ code)")); }
				FReferencerInformationList Holders;
				UObject* Probe = Obj;
				if (IsReferenced(Probe, GARBAGE_COLLECTION_KEEPFLAGS, EInternalObjectFlags_GarbageCollectionKeepFlags, /*bCheckSubObjects*/ true, &Holders))
				{
					TArray<FString> Names;
					for (const FReferencerInformation& H : Holders.ExternalReferences)
					{
						if (!H.Referencer) { continue; }
						FString Entry = FString::Printf(TEXT("%s (%s)"), *H.Referencer->GetPathName(), *H.Referencer->GetClass()->GetName());
						if (H.ReferencingProperties.Num() && H.ReferencingProperties[0]) { Entry += FString::Printf(TEXT(" through %s"), *H.ReferencingProperties[0]->GetName()); }
						Names.Add(Entry);
						if (Names.Num() >= 5) { break; }
					}
					if (Names.Num()) { Why.Add(FString::Printf(TEXT("held in memory by %s"), *FString::Join(Names, TEXT(", ")))); }
				}
			}
		}
		FString File;
		if (EW::PackageFileOnDisk(Package, File) && IFileManager::Get().IsReadOnly(*File))
		{
			Why.Add(TEXT("the package file is read-only (source control checkout needed)"));
		}
		return Why;
	}

	TSharedRef<FJsonObject> ReasonJson(const FString& Package, const TArray<FString>& Why)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("asset"), Package);
		J->SetStringField(TEXT("reason"), Why.Num() ? FString::Join(Why, TEXT("; ")) : FString(TEXT("unknown: nothing in the registry, the editors or the unsaved packages explains it; read log_read for the engine's message")));
		J->SetArrayField(TEXT("details"), ToJson(Why));
		return J;
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

TArray<FString> EW::FindAssets(const FString& Folder, UClass* Class)
{
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*Folder));
	Filter.bRecursivePaths = true;
	if (Class) { Filter.ClassPaths.Add(Class->GetClassPathName()); Filter.bRecursiveClasses = true; }
	TArray<FAssetData> Found;
	Registry().GetAssets(Filter, Found);
	TArray<FString> Out;
	for (const FAssetData& A : Found) { Out.Add(A.PackageName.ToString()); }
	Out.Sort();
	return Out;
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
		TEXT(R"({"type":"object","properties":{"path":{"type":"string","description":"Asset path, e.g. /Game/Items/BP_Apple (with or without the trailing .BP_Apple)."}},"required":["path"]})"),
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
			"path":{"type":"string","description":"Asset path, e.g. /Game/Items/BP_Apple."},
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
			"source":{"type":"string","description":"Asset path, e.g. /Game/Items/BP_Apple."},
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
		TEXT("Rename or move an asset. Every referencer is updated and the redirector left behind is fixed up and removed. Also reports text that still names the old asset (strings, names, IDs, paths in DataTables, Blueprint defaults, data assets): a redirector cannot fix those. dryRun reports referencers and such text without renaming."),
		TEXT(R"({"type":"object","properties":{
			"source":{"type":"string","description":"Asset to rename or move, e.g. /Game/Items/BP_Apple_Old."},
			"destination":{"type":"string","description":"Full new path, e.g. /Game/Items/BP_Apple."},
			"dryRun":{"type":"boolean","description":"Only report what would change (default false)."},
			"textSearchFolder":{"type":"string","description":"Where to look for text naming the old asset; default /Game."}},
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
			const FString OldName = FPackageName::GetShortName(Old);
			FString TextFolder = EW::Str(Args, TEXT("textSearchFolder"));
			if (TextFolder.IsEmpty()) { TextFolder = TEXT("/Game"); }
			// Text that names the old asset: the bare name (IDs, row keys) or its path inside a plain string.
			// Real asset references are excluded: the rename fixes those.
			auto TextRefs = [&Old, &OldName, &TextFolder](bool bAfterRename) -> TArray<TSharedPtr<FJsonValue>>
			{
				TArray<TSharedPtr<FJsonValue>> Out;
				EW::ScanContentText(TextFolder, 5000, [&](const FString& A, const FString& W, const FString& F, const FString& V)
				{
					if (Out.Num() >= 200 || A == Old) { return; }
					const bool bName = V.Equals(OldName, ESearchCase::IgnoreCase);
					const bool bPath = bAfterRename && (V.Equals(Old) || V.StartsWith(Old + TEXT(".")));
					if (!bName && !bPath) { return; }
					TSharedRef<FJsonObject> H = MakeShared<FJsonObject>();
					H->SetStringField(TEXT("asset"), A);
					H->SetStringField(TEXT("where"), W);
					H->SetStringField(TEXT("field"), F);
					H->SetStringField(TEXT("value"), V);
					Out.Add(MakeShared<FJsonValueObject>(H));
				});
				return Out;
			};

			if (EW::Bool(Args, TEXT("dryRun"), false))
			{
				TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetBoolField(TEXT("dryRun"), true);
				Data->SetStringField(TEXT("from"), Old);
				Data->SetStringField(TEXT("to"), Dest);
				Data->SetArrayField(TEXT("referencersToUpdate"), ToJson(Referencers(Old)));
				const TArray<TSharedPtr<FJsonValue>> Text = TextRefs(false);
				Data->SetArrayField(TEXT("textReferences"), Text);
				FEWResult Res = FEWResult::Ok(Data, TEXT("Nothing was renamed."));
				if (Text.Num()) { Res.Warnings.Add(FString::Printf(TEXT("%d text value(s) name '%s'; the rename will not change them."), Text.Num(), *OldName)); }
				return Res;
			}

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
			else
			{
				// Referencers are fixed and the redirector is gone: forget the in-session path redirection too, so a
				// new asset created later at the old path is not silently redirected to the moved one.
				const FString OldPkg = FPackageName::ObjectPathToPackageName(OldObjectPath);
				const FString OldShort = FPackageName::GetShortName(OldPkg);
				GRedirectCollector.RemoveAssetPathRedirection(FSoftObjectPath(OldPkg + TEXT(".") + OldShort));
				GRedirectCollector.RemoveAssetPathRedirection(FSoftObjectPath(OldPkg + TEXT(".") + OldShort + TEXT("_C")));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("path"), Dest);
			Data->SetBoolField(TEXT("exists"), AssetExists(Dest));
			Data->SetArrayField(TEXT("referencersUpdated"), ToJson(Referencers(Dest)));
			const TArray<TSharedPtr<FJsonValue>> Text = TextRefs(true);
			Data->SetArrayField(TEXT("textReferences"), Text);
			if (Text.Num()) { Res.Warnings.Add(FString::Printf(TEXT("%d text value(s) still name the old asset (see textReferences); update them by hand."), Text.Num())); }
			Res.Data = Data;
			return Res;
		}, /*bTransactional*/ false);

	R.Register(TEXT("asset_delete"),
		TEXT("Delete assets. Refused (ASSET_REFERENCED) while an asset outside the deleted set still references them: data.blocking and data.reasons say which asset or package uses each one. "
			"An asset that is still there after the delete returns DELETE_INCOMPLETE with a reason per asset in data.reasons (open in an asset editor, unsaved changes, referenced by another asset, read-only file, or the objects that hold the loaded asset in memory). "
			"A set whose assets only reference each other is deleted in one call (referencers first). String Tables are deleted too (the engine keeps loaded String Tables in memory; the reference check above still applies). "
			"Copies the engine keeps in memory (for example the transient re-instancing copies of an edited struct) do not block the delete. "
			"force true skips the reference check: it closes the editors of the referencing assets, deletes the placed instances of a deleted Blueprint, nulls the references inside loaded assets (they become unsaved, Enginewright does not save them) and leaves references in assets that are not loaded dangling until those are opened and saved. It does not free an asset that something else still holds in memory: that ends in DELETE_INCOMPLETE with the reason."),
		TEXT(R"({"type":"object","properties":{
			"paths":{"type":"array","items":{"type":"string"},"description":"Asset paths, e.g. [\"/Game/Items/BP_Apple\"]."},
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
				TArray<TSharedPtr<FJsonValue>> Reasons;
				for (UObject* Obj : Objects)
				{
					const FString Package = Obj->GetOutermost()->GetName();
					const TArray<FString> Why = WhyAssetStays(Package, Obj, Set, /*bMemory*/ false);
					if (Why.ContainsByPredicate([](const FString& W) { return W.StartsWith(TEXT("referenced by")); })) { Reasons.Add(MakeShared<FJsonValueObject>(ReasonJson(Package, Why))); }
				}
				Data->SetArrayField(TEXT("reasons"), Reasons);
				Res.Data = Data;
				return Res;
			}

			int32 Deleted = 0;
			if (bForce)
			{
				Deleted = ObjectTools::ForceDeleteObjects(Objects, false);
			}
			else
			{
				// Remember the objects by path, not by pointer: the garbage collection below frees the deleted package.
				TMap<FString, TArray<FString>> PathsByPackage;
				for (UObject* Obj : Objects) { PathsByPackage.FindOrAdd(Obj->GetOutermost()->GetName()).AddUnique(Obj->GetPathName()); }
				// Referencers first, so an asset is not held in memory by another one of the same set.
				const TArray<FString> Order = ReferencersFirst(Set, Objects);
				Objects.Reset();
				for (const FString& Package : Order)
				{
					bool bDeletedHere = false;
					if (const TArray<FString>* PkgPaths = PathsByPackage.Find(Package))
					{
						for (const FString& Path : *PkgPaths)
						{
							UObject* Obj = FindObject<UObject>(nullptr, *Path);
							if (!Obj) { continue; }
							// A loaded String Table stays alive until it is marked garbage: force delete that one.
							int32 N = Obj->IsA<UStringTable>() ? ObjectTools::ForceDeleteObjects({ Obj }, false) : ObjectTools::DeleteObjects({ Obj }, false);
							// The engine keeps transient copies of an edited struct (re-instancing) that only point at it: a normal
							// delete refuses those, but nothing outside the set really uses the asset, so force delete that one.
							if (N == 0 && !Obj->IsA<UStringTable>())
							{
								FReferencerInformationList Holders;
								UObject* Probe = Obj;
								if (IsReferenced(Probe, GARBAGE_COLLECTION_KEEPFLAGS, EInternalObjectFlags_GarbageCollectionKeepFlags, /*bCheckSubObjects*/ true, &Holders))
								{
									int32 ExternalHolders = 0;
									bool bHeldByTransientOrSetOnly = true;
									for (const FReferencerInformation& H : Holders.ExternalReferences)
									{
										if (!H.Referencer) { continue; }
										ExternalHolders++;
										if (H.Referencer->GetOutermost() != GetTransientPackage() && !Set.Contains(H.Referencer->GetOutermost()->GetName()))
										{
											bHeldByTransientOrSetOnly = false;
											break;
										}
									}
									if (ExternalHolders && bHeldByTransientOrSetOnly)
									{
										N = ObjectTools::ForceDeleteObjects({ Obj }, false);
									}
								}
							}
							Deleted += N;
							bDeletedHere = bDeletedHere || N > 0;
						}
					}
					// The deleted package stays in memory (and keeps its references) until a garbage collection.
					if (bDeletedHere) { CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS); }
				}
			}
			Objects.Reset();
			// Free the deleted packages now, so the same path can be created again in this session.
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			TArray<FString> Left;
			TArray<TSharedPtr<FJsonValue>> LeftReasons;
			for (const FString& Package : Set)
			{
				TArray<FAssetData> Still;
				Registry().GetAssetsByPackageName(FName(*Package), Still, false);
				UPackage* StillLoaded = FindPackage(nullptr, *Package);
				UObject* StillAsset = StillLoaded ? StaticFindObject(UObject::StaticClass(), StillLoaded, *FPackageName::GetShortName(Package)) : nullptr;
				FString PkgFile;
				const bool bFileThere = EW::PackageFileOnDisk(Package, PkgFile);
				if (Still.Num() == 0 && !bFileThere && IsValid(StillAsset))
				{
					// Deleted from the registry and from disk, but something (undo buffer, an open tool) still holds the
					// object. Move it out of the way so the path is really free again.
					StillAsset->Rename(*MakeUniqueObjectName(GetTransientPackage(), StillAsset->GetClass(), FName(*(TEXT("EWDeleted_") + StillAsset->GetName()))).ToString(),
						GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
					StillAsset->MarkAsGarbage();
					StillAsset = nullptr;
				}
				if (Still.Num() > 0 || IsValid(StillAsset))
				{
					Left.Add(Package);
					LeftReasons.Add(MakeShared<FJsonValueObject>(ReasonJson(Package, WhyAssetStays(Package, IsValid(StillAsset) ? StillAsset : nullptr, Set, /*bMemory*/ true))));
					continue;
				}
				// The engine skips the file when its package-name cache says the package is gone (for example a
				// package deleted and created again in this session). The package is unloaded here, so remove
				// the leftover file ourselves; otherwise the path stays taken until the editor restarts.
				FString File;
				if (EW::PackageFileOnDisk(Package, File))
				{
					if (!IFileManager::Get().Delete(*File, false, false, true))
					{
						Left.Add(Package);
						TArray<FString> Why = WhyAssetStays(Package, nullptr, Set, false);
						Why.Add(FString::Printf(TEXT("the file %s could not be deleted (locked by another program or read-only)"), *File));
						LeftReasons.Add(MakeShared<FJsonValueObject>(ReasonJson(Package, Why)));
					}
				}
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("deleted"), Deleted);
			Data->SetArrayField(TEXT("stillThere"), ToJson(Left));
			Data->SetArrayField(TEXT("reasons"), LeftReasons);
			if (Left.Num())
			{
				FEWResult Res = FEWResult::Error(TEXT("DELETE_INCOMPLETE"), TEXT("Some assets are still there; data.reasons gives the reason for each one (open in an editor, unsaved changes, referenced, held in memory, file locked)."));
				Res.Data = Data;
				return Res;
			}
			FEWResult Res = FEWResult::Ok(Data);
			if (Blocking.Num()) { Res.Warnings.Add(FString::Printf(TEXT("Forced: %d reference(s) are now broken."), Blocking.Num())); }
			return Res;
		}, /*bTransactional*/ false);

	R.Register(TEXT("asset_save"),
		TEXT("Save assets by path, or every unsaved asset and level when paths is empty."),
		TEXT(R"({"type":"object","properties":{"paths":{"type":"array","items":{"type":"string"},"description":"Asset paths to save, e.g. [\"/Game/Items/BP_Apple\"]. Empty or omitted saves every unsaved asset and level."}}})"),
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
