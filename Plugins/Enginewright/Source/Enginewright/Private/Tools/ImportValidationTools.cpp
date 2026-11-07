#include "EnginewrightTool.h"
#include "EWCommon.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "EditorValidatorSubsystem.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"

namespace
{
	FEWResult Bad(const FString& Code, const FString& Msg, const TArray<FString>& Sug = {}) { return FEWResult::Error(Code, Msg, Sug); }
	IAssetRegistry& Registry() { return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get(); }

	TArray<TSharedPtr<FJsonValue>> Strings(const TArray<FString>& In, int32 Max = 200)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& S : In) { if (Out.Num() >= Max) { break; } Out.Add(MakeShared<FJsonValueString>(S)); }
		return Out;
	}
}

void RegisterImportTools(FEWRegistry& R)
{
	R.Register(TEXT("asset_import"),
		TEXT("Import files into the open editor: textures (png, jpg, tga, exr), sounds (wav, ogg), fonts (ttf, otf), meshes (fbx, obj, gltf). Unattended (no dialogs). Each file must exist; existing assets are only replaced with replace true. Returns the created asset paths per file and saves them."),
		TEXT(R"({"type":"object","properties":{
			"files":{"type":"array","items":{"type":"string"},"description":"Absolute file paths on disk."},
			"folder":{"type":"string","description":"Destination folder, e.g. /Game/Textures."},
			"replace":{"type":"boolean","description":"Replace assets that already exist (default false)."}},
			"required":["files","folder"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const TArray<FString> Files = EW::StrList(Args, TEXT("files"));
			FString Folder = EW::Str(Args, TEXT("folder"));
			Folder.RemoveFromEnd(TEXT("/"));
			const bool bReplace = EW::Bool(Args, TEXT("replace"), false);
			if (Files.Num() == 0) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("files is empty.")); }
			if (!Folder.StartsWith(TEXT("/Game")) || !FPackageName::IsValidLongPackageName(Folder, false)) { return Bad(TEXT("INVALID_PATH"), TEXT("folder must be a /Game/... folder.")); }
			static const TArray<FString> Ext = { TEXT("png"), TEXT("jpg"), TEXT("jpeg"), TEXT("tga"), TEXT("bmp"), TEXT("exr"), TEXT("hdr"), TEXT("psd"), TEXT("wav"), TEXT("ogg"), TEXT("flac"),
				TEXT("ttf"), TEXT("otf"), TEXT("fbx"), TEXT("obj"), TEXT("gltf"), TEXT("glb"), TEXT("csv"), TEXT("json") };
			// Check every file and target first.
			for (const FString& F : Files)
			{
				if (!FPaths::FileExists(F)) { return Bad(TEXT("FILE_NOT_FOUND"), FString::Printf(TEXT("No file '%s'. Nothing was imported."), *F)); }
				const FString E = FPaths::GetExtension(F).ToLower();
				if (!Ext.Contains(E)) { return Bad(TEXT("UNSUPPORTED_FILE"), FString::Printf(TEXT("'%s': .%s is not a supported import type (%s). Nothing was imported."), *F, *E, *FString::Join(Ext, TEXT(", ")))); }
				const FString Target = Folder / ObjectTools::SanitizeObjectName(FPaths::GetBaseFilename(F));
				if (!bReplace && EW::AssetPathTaken(Target)) { return Bad(TEXT("ASSET_EXISTS"), FString::Printf(TEXT("'%s' already exists (pass replace true). Nothing was imported."), *Target)); }
			}
			TArray<UAssetImportTask*> Tasks;
			for (const FString& F : Files)
			{
				UAssetImportTask* T = NewObject<UAssetImportTask>();
				T->Filename = FPaths::ConvertRelativePathToFull(F);
				T->DestinationPath = Folder;
				T->bAutomated = true;
				T->bReplaceExisting = bReplace;
				T->bReplaceExistingSettings = false;
				T->bSave = true;
				Tasks.Add(T);
			}
			FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().ImportAssetTasks(Tasks);
			TArray<TSharedPtr<FJsonValue>> Results;
			int32 Failed = 0;
			for (UAssetImportTask* T : Tasks)
			{
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("file"), T->Filename);
				TArray<FString> Paths;
				for (UObject* O : T->GetObjects()) { if (O) { Paths.Add(O->GetPathName()); } }
				if (Paths.Num() == 0) { for (const FString& P : T->ImportedObjectPaths) { Paths.Add(P); } }
				J->SetArrayField(TEXT("assets"), Strings(Paths));
				if (Paths.Num() == 0) { ++Failed; J->SetStringField(TEXT("error"), TEXT("Nothing was imported from this file (see log_read for the importer's message).")); }
				Results.Add(MakeShared<FJsonValueObject>(J));
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("results"), Results);
			FEWResult Res = Failed ? Bad(TEXT("IMPORT_FAILED"), FString::Printf(TEXT("%d of %d file(s) did not import."), Failed, Files.Num())) : FEWResult::Ok();
			Res.Data = Data;
			return Res;
		}, /*bTransactional*/ false);
}

void RegisterValidationTools(FEWRegistry& R)
{
	R.Register(TEXT("validate"),
		TEXT("Check content for problems before they show up in play: checks blueprints (compile every Blueprint), data (Data Validation rules), references (assets that point to missing packages), map (Map Check of the open level). Default: all. Reports each problem; ok is false when any error is found."),
		TEXT(R"({"type":"object","properties":{
			"folder":{"type":"string","description":"Content folder to check, default /Game."},
			"checks":{"type":"array","items":{"type":"string","enum":["blueprints","data","references","map"]}}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			if (EW::IsPlaying()) { return Bad(TEXT("PIE_RUNNING"), TEXT("Stop Play In Editor first (pie_stop).")); }
			FString Folder = EW::Str(Args, TEXT("folder"));
			if (Folder.IsEmpty()) { Folder = TEXT("/Game"); }
			Folder.RemoveFromEnd(TEXT("/"));
			TArray<FString> Checks = EW::StrList(Args, TEXT("checks"));
			if (Checks.Num() == 0) { Checks = { TEXT("blueprints"), TEXT("data"), TEXT("references"), TEXT("map") }; }
			static const TArray<FString> Known = { TEXT("blueprints"), TEXT("data"), TEXT("references"), TEXT("map") };
			for (const FString& Ck : Checks)
			{
				if (!Known.Contains(Ck)) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Unknown check '%s'. Checks: %s."), *Ck, *FString::Join(Known, TEXT(", "))), EW::Suggest(Ck, Known)); }
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			int32 Errors = 0;

			if (Checks.Contains(TEXT("blueprints")))
			{
				TSharedRef<FJsonObject> A = MakeShared<FJsonObject>();
				A->SetStringField(TEXT("folder"), Folder);
				const TArray<FString> BPs = EW::FindAssets(Folder, UBlueprint::StaticClass());
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				if (BPs.Num())
				{
					const FEWResult C = FEWRegistry::Get().Call(TEXT("blueprint_compile"), A);
					J = C.Data.IsValid() ? C.Data.ToSharedRef() : J;
					double F = 0; J->TryGetNumberField(TEXT("failed"), F);
					Errors += (int32)F;
				}
				else { J->SetNumberField(TEXT("compiled"), 0); }
				Data->SetObjectField(TEXT("blueprints"), J);
			}

			if (Checks.Contains(TEXT("data")))
			{
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				UEditorValidatorSubsystem* V = GEditor->GetEditorSubsystem<UEditorValidatorSubsystem>();
				if (!V) { J->SetStringField(TEXT("skipped"), TEXT("Data Validation plugin is not enabled.")); }
				else
				{
					FARFilter Filter;
					Filter.PackagePaths.Add(FName(*Folder));
					Filter.bRecursivePaths = true;
					TArray<FAssetData> Assets;
					Registry().GetAssets(Filter, Assets);
					FValidateAssetsSettings Settings;
					Settings.bSkipExcludedDirectories = true;
					Settings.bShowIfNoFailures = false;
					Settings.ValidationUsecase = EDataValidationUsecase::Script;
					FValidateAssetsResults Results;
					const uint64 Seq = EW::LogSeq();
					V->ValidateAssetsWithSettings(Assets, Settings, Results);
					J->SetNumberField(TEXT("checked"), Results.NumChecked);
					J->SetNumberField(TEXT("invalid"), Results.NumInvalid);
					J->SetNumberField(TEXT("withWarnings"), Results.NumWarnings);
					TArray<FString> Lines;
					for (const FString& L : EW::LogLinesSince(Seq)) { if (L.Contains(TEXT("Error")) || L.Contains(TEXT("Warning")) || L.Contains(TEXT("invalid"))) { Lines.Add(L); } }
					J->SetArrayField(TEXT("messages"), Strings(Lines, 60));
					Errors += Results.NumInvalid;
				}
				Data->SetObjectField(TEXT("data"), J);
			}

			if (Checks.Contains(TEXT("references")))
			{
				TArray<FString> Broken;
				FARFilter Filter;
				Filter.PackagePaths.Add(FName(*Folder));
				Filter.bRecursivePaths = true;
				TArray<FAssetData> Assets;
				Registry().GetAssets(Filter, Assets);
				TSet<FName> Seen;
				for (const FAssetData& A : Assets)
				{
					if (Seen.Contains(A.PackageName)) { continue; }
					Seen.Add(A.PackageName);
					TArray<FName> Deps;
					Registry().GetDependencies(A.PackageName, Deps, UE::AssetRegistry::EDependencyCategory::Package);
					for (const FName& D : Deps)
					{
						const FString S = D.ToString();
						if (!S.StartsWith(TEXT("/Game/"))) { continue; }
						TArray<FAssetData> Found;
						Registry().GetAssetsByPackageName(D, Found, false);
						if (Found.Num() == 0 && !FPackageName::DoesPackageExist(S)) { Broken.Add(A.PackageName.ToString() + TEXT(" -> ") + S); }
					}
				}
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetNumberField(TEXT("packagesChecked"), Seen.Num());
				J->SetArrayField(TEXT("broken"), Strings(Broken));
				Data->SetObjectField(TEXT("references"), J);
				Errors += Broken.Num();
			}

			if (Checks.Contains(TEXT("map")))
			{
				const uint64 Seq = EW::LogSeq();
				GEditor->Exec(GEditor->GetEditorWorldContext().World(), TEXT("MAP CHECK"));
				TArray<FString> Lines = EW::LogLinesSince(Seq, TEXT("MapCheck"));
				TArray<FString> Problems;
				int32 MapErrors = 0;
				for (const FString& L : Lines)
				{
					if (L.Contains(TEXT("Map check complete")) || L.Contains(TEXT("MapCheck: Map check"))) { continue; }
					if (L.Contains(TEXT("Error"))) { ++MapErrors; }
					Problems.Add(L);
				}
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("map"), GEditor->GetEditorWorldContext().World()->GetOutermost()->GetName());
				J->SetArrayField(TEXT("messages"), Strings(Problems, 60));
				J->SetNumberField(TEXT("errors"), MapErrors);
				Data->SetObjectField(TEXT("map"), J);
				Errors += MapErrors;
			}

			Data->SetNumberField(TEXT("errors"), Errors);
			FEWResult Res = Errors ? Bad(TEXT("VALIDATION_FAILED"), FString::Printf(TEXT("%d problem(s) found; see data."), Errors)) : FEWResult::Ok();
			Res.Data = Data;
			return Res;
		});
}
