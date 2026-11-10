#include "EnginewrightTool.h"
#include "EWCommon.h"

#include "AssetImportTask.h"
#include "AssetCompilingManager.h"
#include "StaticMeshOperations.h"
#include "MeshDescription.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "EditorValidatorSubsystem.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "EWProps.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"

namespace
{
	FEWResult Bad(const FString& Code, const FString& Msg, const TArray<FString>& Sug = {}) { return FEWResult::Error(Code, Msg, Sug); }
	IAssetRegistry& Registry() { return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get(); }

	bool IsMeshFile(const FString& File)
	{
		const FString E = FPaths::GetExtension(File).ToLower();
		return E == TEXT("fbx") || E == TEXT("obj") || E == TEXT("gltf") || E == TEXT("glb");
	}

	/** True when the file holds a skin (FBX: a Deformer record, glTF: "skins"). OBJ has no skeletons. */
	bool LooksSkeletal(const FString& File)
	{
		const FString E = FPaths::GetExtension(File).ToLower();
		if (E == TEXT("obj")) { return false; }
		if (IFileManager::Get().FileSize(*File) > 200 * 1024 * 1024) { return false; }
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *File)) { return false; }
		auto Has = [&Bytes](const char* Needle)
		{
			const int32 N = FCStringAnsi::Strlen(Needle);
			for (int32 i = 0; i + N <= Bytes.Num(); ++i)
			{
				if (Bytes[i] == (uint8)Needle[0] && FMemory::Memcmp(&Bytes[i], Needle, N) == 0) { return true; }
			}
			return false;
		};
		return E == TEXT("fbx") ? Has("Deformer") : Has("\"skins\"");
	}

	bool ReadVec3(const TSharedPtr<FJsonValue>& V, FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (!V.IsValid() || !V->TryGetArray(A) || A->Num() != 3) { return false; }
		double X, Y, Z;
		if (!(*A)[0]->TryGetNumber(X) || !(*A)[1]->TryGetNumber(Y) || !(*A)[2]->TryGetNumber(Z)) { return false; }
		Out = FVector(X, Y, Z);
		return FMath::IsFinite(X) && FMath::IsFinite(Y) && FMath::IsFinite(Z);
	}

	TArray<TSharedPtr<FJsonValue>> Vec3Json(const FVector& V)
	{
		return { MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.X * 100) / 100), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Y * 100) / 100), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Z * 100) / 100) };
	}

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
		TEXT("Import files into the open editor: textures (png, jpg, tga, exr), sounds (wav, ogg), fonts (ttf, otf), meshes (fbx, obj, gltf). Unattended (no dialogs). Each file must exist; existing assets are only replaced with replace true. Returns the created asset paths per file and saves them. Mesh files can be scaled, rotated and moved while they are imported: scale (one number, or [x, y, z]), rotation ([pitch, yaw, roll] degrees) and translation ([x, y, z] cm) are the import transform of the importer; meshType auto looks for a skin in the file, and says static or skeletal when you know. A different x, y and z scale works for static meshes only (it is applied as the build scale of the mesh). Each imported mesh is returned with its size in cm. These arguments are refused for files that are not meshes."),
		TEXT(R"({"type":"object","properties":{
			"files":{"type":"array","items":{"type":"string"},"description":"Absolute file paths on disk."},
			"folder":{"type":"string","description":"Destination folder, e.g. /Game/Textures."},
			"replace":{"type":"boolean","description":"Replace assets that already exist (default false)."},
			"scale":{"type":["number","array"],"description":"Import scale for mesh files: one number (uniform), or [x, y, z] (static meshes only when the three differ). Default 1."},
			"rotation":{"type":"array","items":{"type":"number"},"description":"Import rotation of mesh files, [pitch, yaw, roll] in degrees."},
			"translation":{"type":"array","items":{"type":"number"},"description":"Import offset of mesh files, [x, y, z] in cm."},
			"meshType":{"type":"string","enum":["auto","static","skeletal"],"description":"What a mesh file is imported as; auto (default) looks for a skin in the file."}},
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
			// Import transform of mesh files (checked before anything is imported).
			FVector Scale3(1, 1, 1), Rotation3(0, 0, 0), Translation3(0, 0, 0);
			bool bScale = false, bRotation = false, bTranslation = false;
			const TSharedPtr<FJsonValue> ScaleArg = Args->TryGetField(TEXT("scale"));
			if (ScaleArg.IsValid() && !ScaleArg->IsNull())
			{
				double One = 0;
				if (ScaleArg->TryGetNumber(One)) { Scale3 = FVector(One); }
				else if (!ReadVec3(ScaleArg, Scale3)) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("scale must be one number or [x, y, z]. Nothing was imported.")); }
				if (Scale3.X <= 0 || Scale3.Y <= 0 || Scale3.Z <= 0 || !FMath::IsFinite(Scale3.X)) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("scale must be greater than 0. Nothing was imported.")); }
				bScale = true;
			}
			if (Args->HasField(TEXT("rotation"))) { if (!ReadVec3(Args->TryGetField(TEXT("rotation")), Rotation3)) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("rotation must be [pitch, yaw, roll] (three numbers). Nothing was imported.")); } bRotation = true; }
			if (Args->HasField(TEXT("translation"))) { if (!ReadVec3(Args->TryGetField(TEXT("translation")), Translation3)) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("translation must be [x, y, z] (three numbers). Nothing was imported.")); } bTranslation = true; }
			const FString MeshTypeArg = EW::Str(Args, TEXT("meshType"));
			const bool bTransform = bScale || bRotation || bTranslation || !MeshTypeArg.IsEmpty();
			if (bTransform)
			{
				for (const FString& F : Files)
				{
					if (!IsMeshFile(F)) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("scale, rotation, translation and meshType are for mesh files (fbx, obj, gltf, glb); '%s' is not one. Import it in a separate call. Nothing was imported."), *F)); }
				}
			}
			const bool bUniform = FMath::IsNearlyEqual(Scale3.X, Scale3.Y) && FMath::IsNearlyEqual(Scale3.Y, Scale3.Z);
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
			TArray<bool> SkeletalFile;
			for (const FString& F : Files)
			{
				UAssetImportTask* T = NewObject<UAssetImportTask>();
				bool bSkeletal = false;
				if (bTransform)
				{
					bSkeletal = MeshTypeArg.Equals(TEXT("skeletal"), ESearchCase::IgnoreCase) || (!MeshTypeArg.Equals(TEXT("static"), ESearchCase::IgnoreCase) && LooksSkeletal(F));
					if (bSkeletal && bScale && !bUniform) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("'%s' is a skeletal mesh: only one scale for all axes is possible (scale [x, y, z] needs x = y = z). Nothing was imported."), *F)); }
					// The same options object the Import dialog fills; Enginewright sets only the transform.
					UFbxImportUI* UI = NewObject<UFbxImportUI>(T);
					UI->bImportMesh = true;
					UI->bImportAsSkeletal = bSkeletal;
					UI->bAutomatedImportShouldDetectType = false;
					UI->MeshTypeToImport = bSkeletal ? FBXIT_SkeletalMesh : FBXIT_StaticMesh;
					UFbxAssetImportData* Data = bSkeletal ? static_cast<UFbxAssetImportData*>(UI->SkeletalMeshImportData) : static_cast<UFbxAssetImportData*>(UI->StaticMeshImportData);
					if (bScale && bUniform) { Data->ImportUniformScale = (float)Scale3.X; }
					if (bRotation) { Data->ImportRotation = FRotator(Rotation3.X, Rotation3.Y, Rotation3.Z); }
					if (bTranslation) { Data->ImportTranslation = Translation3; }
					T->Options = UI;
				}
				SkeletalFile.Add(bSkeletal);
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
				if (bTransform)
				{
					// A scale with different x, y and z cannot be given to the importer: apply it as the build scale of the static mesh.
					TArray<TSharedPtr<FJsonValue>> Meshes;
					for (UObject* O : T->GetObjects())
					{
						if (UStaticMesh* SM = Cast<UStaticMesh>(O))
						{
							if (bScale && !bUniform)
							{
								SM->Modify();
								for (int32 L = 0; L < SM->GetNumSourceModels(); ++L)
								{
									// Bake the scale into the vertices of the mesh description (BuildScale3D alone does not move the bounds).
									if (FMeshDescription* MD = SM->GetMeshDescription(L))
									{
										FStaticMeshOperations::ApplyTransform(*MD, FTransform(FQuat::Identity, FVector::ZeroVector, Scale3), true);
										SM->CommitMeshDescription(L);
									}
								}
								SM->PostEditChange(); // rebuilds the mesh with the new build scale
								FAssetCompilingManager::Get().FinishAllCompilation(); // the build can be asynchronous: wait, so the size below is the scaled one
								SM->MarkPackageDirty();
								bool bDeferred = false;
								EW::SaveOrDefer(SM->GetOutermost(), bDeferred);
							}
							TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
							M->SetStringField(TEXT("asset"), SM->GetPathName());
							M->SetStringField(TEXT("kind"), TEXT("static"));
							M->SetArrayField(TEXT("size"), Vec3Json(SM->GetBoundingBox().GetSize()));
							Meshes.Add(MakeShared<FJsonValueObject>(M));
						}
						else if (USkeletalMesh* SK = Cast<USkeletalMesh>(O))
						{
							TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
							M->SetStringField(TEXT("asset"), SK->GetPathName());
							M->SetStringField(TEXT("kind"), TEXT("skeletal"));
							M->SetArrayField(TEXT("size"), Vec3Json(SK->GetBounds().GetBox().GetSize()));
							Meshes.Add(MakeShared<FJsonValueObject>(M));
						}
					}
					J->SetArrayField(TEXT("meshes"), Meshes);
				}
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
			"checks":{"type":"array","items":{"type":"string","enum":["blueprints","data","references","map"]},"description":"Checks to run: blueprints, data, references, map. Default: all."}}})"),
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
