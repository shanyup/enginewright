// actor_preview: build a class that sets itself up in code (construction, BeginPlay) in the editor and photograph it, no game run.
#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EWProps.h"

#include "Camera/CameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/Base64.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PreviewScene.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "TextureResource.h"
#include "UObject/Package.h"

namespace
{
	FEWResult Bad(const FString& Code, const FString& Msg, const TArray<FString>& Sug = {}) { return FEWResult::Error(Code, Msg, Sug); }

	TArray<TSharedPtr<FJsonValue>> Vec(const FVector& V)
	{
		return { MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.X * 10) / 10), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Y * 10) / 10), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Z * 10) / 10) };
	}

	bool ReadVec3(const TSharedPtr<FJsonObject>& A, const TCHAR* Key, FVector& Out, bool& bPresent, FString& Err)
	{
		bPresent = false;
		if (!A->HasField(Key)) { return true; }
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!A->TryGetArrayField(Key, Arr) || Arr->Num() != 3) { Err = FString::Printf(TEXT("%s must be an array of 3 numbers."), Key); return false; }
		for (const auto& V : *Arr) { if (V->Type != EJson::Number) { Err = FString::Printf(TEXT("%s must hold numbers only."), Key); return false; } }
		Out = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
		bPresent = true;
		return true;
	}

	/** Visible bounds of the given actors (editor-only parts left out). */
	FBox VisibleBounds(const TArray<AActor*>& Actors)
	{
		FBox Box(ForceInit);
		for (AActor* A : Actors)
		{
			if (!IsValid(A)) { continue; }
			TArray<UPrimitiveComponent*> Prims;
			A->GetComponents(Prims, true);
			for (UPrimitiveComponent* C : Prims)
			{
				if (!C || !C->IsRegistered() || C->IsEditorOnly() || !C->IsVisible() || C->bHiddenInGame) { continue; }
				if (C->Bounds.SphereRadius <= 0) { continue; }
				Box += C->Bounds.GetBox();
			}
		}
		return Box;
	}

	struct FParams
	{
		UClass* Class = nullptr;
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		TSharedPtr<FJsonObject> Props;
		bool bBeginPlay = true;
		int32 Ticks = 3;
		bool bLevel = false;
		bool bGround = false;
		FString Lighting = TEXT("default");
		int32 Width = 1280, Height = 720;
		FString View = TEXT("iso");
		double Fov = 50, Margin = 1.15;
		bool bYaw = false, bPitch = false, bDistance = false, bCamLoc = false, bLookAt = false, bExposure = false;
		double Yaw = 0, Pitch = 0, Distance = 0, Exposure = 5;
		FVector CamLoc = FVector::ZeroVector, LookAt = FVector::ZeroVector;
		AActor* CameraActor = nullptr;
	};

	/** Spawns the class like the game would (construction script, optional BeginPlay). New actors, including those the class spawned itself, go to OutNew. */
	AActor* SpawnPreview(UWorld* World, const FParams& P, TArray<AActor*>& OutNew, FEWResult& OutErr)
	{
		TSet<AActor*> Before;
		for (TActorIterator<AActor> It(World); It; ++It) { Before.Add(*It); }

		FActorSpawnParameters SP;
		SP.ObjectFlags = RF_Transient;
		SP.bTemporaryEditorActor = true;
		SP.bHideFromSceneOutliner = true;
		SP.bDeferConstruction = true;
		SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		const FTransform Xf(P.Rotation, P.Location);
		AActor* A = World->SpawnActor<AActor>(P.Class, Xf, SP);
		if (!A) { OutErr = Bad(TEXT("SPAWN_FAILED"), FString::Printf(TEXT("The engine could not spawn %s."), *P.Class->GetName())); return nullptr; }
		if (P.Props.IsValid() && P.Props->Values.Num() > 0)
		{
			FEWResult W = EW::WriteValues(P.Class, A, nullptr, P.Props);
			if (!W.bOk) { World->DestroyActor(A); OutErr = Bad(W.ErrorCode, TEXT("properties: ") + W.Message, W.Suggestions); return nullptr; }
		}
		A->FinishSpawning(Xf);
		if (P.bBeginPlay)
		{
			if (!World->HasBegunPlay())
			{
				if (AWorldSettings* WS = World->GetWorldSettings()) { WS->NotifyBeginPlay(); }
			}
			// Actors the class spawned itself may still wait for BeginPlay (editor world: nobody calls it).
			for (int32 Pass = 0; Pass < 4; ++Pass)
			{
				bool bAny = false;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (!Before.Contains(*It) && IsValid(*It) && !It->HasActorBegunPlay() && !It->IsActorBeginningPlay()) { It->DispatchBeginPlay(); bAny = true; }
				}
				if (!bAny) { break; }
			}
		}
		for (TActorIterator<AActor> It(World); It; ++It) { if (!Before.Contains(*It) && IsValid(*It)) { OutNew.Add(*It); } }
		return A;
	}

	void DestroyAll(UWorld* World, TArray<AActor*>& Actors)
	{
		for (int32 i = Actors.Num() - 1; i >= 0; --i)
		{
			if (IsValid(Actors[i]))
			{
				Actors[i]->RouteEndPlay(EEndPlayReason::Destroyed);
				World->DestroyActor(Actors[i]);
			}
		}
		Actors.Reset();
	}
}

void RegisterActorPreviewTools(FEWRegistry& R)
{
	R.Register(TEXT("actor_preview"),
		TEXT("Build an actor class the way the game does (construction script, then BeginPlay when beginPlay is on) and take a PNG of it from a chosen camera, without Play In Editor or game_run. ")
		TEXT("Use it for classes that dress themselves in code (stations, dioramas, decks, props built in BeginPlay): change the code or the data, call actor_preview, look. A game_run shot takes minutes; this takes seconds. ")
		TEXT("world isolated (default): a private preview world with a sun, sky light and an optional ground plane; the open level is not touched. ")
		TEXT("world level: spawns the class temporarily in the open level (its lighting, post process volumes and neighbours show) and removes it afterwards, so the image matches a game_run shot better; the level and the undo stack are not changed. ")
		TEXT("There is no game mode, player or HUD in the preview, so code that needs a player controller sees none. The camera frames the whole result by default (view iso, front, back, left, right, top), or give cameraLocation + lookAt, or take another actor's view (cameraActor). Saves the PNG under Saved/Enginewright/Screenshots and returns it as an image. For a UMG screen use widget_render; for the real game use game_run."),
		TEXT(R"({"type":"object","properties":{
			"class":{"type":"string","description":"Actor class: Blueprint path (/Game/Stations/BP_Crate), /Script/Module.Class or short class name."},
			"world":{"type":"string","enum":["isolated","level"],"description":"isolated (default) or level (see description)."},
			"location":{"type":"array","items":{"type":"number"},"description":"[x,y,z] where the actor is built, default the origin."},
			"rotation":{"type":"array","items":{"type":"number"},"description":"[pitch,yaw,roll], default zero."},
			"properties":{"type":"object","description":"Property values set before the construction script runs, like expose-on-spawn: {name: value}, checked like object_set."},
			"beginPlay":{"type":"boolean","description":"Run BeginPlay (default true). Turn off to see only the construction script."},
			"ticks":{"type":"integer","description":"isolated: world ticks after BeginPlay (0 to 60, default 3) so timers and tick-built parts can run."},
			"ground":{"type":"boolean","description":"isolated: add a large floor under the actor (default false)."},
			"lighting":{"type":"string","enum":["default","none"],"description":"isolated: default = sun and sky light; none = only what the class brings."},
			"exposure":{"type":"number","description":"isolated: fixed exposure in EV100, default 5 (lower is brighter). world level: exposure bias in EV added to the level's own exposure (default none)."},
			"width":{"type":"integer","description":"Default 1280."},"height":{"type":"integer","description":"Default 720."},
			"view":{"type":"string","enum":["iso","front","back","left","right","top"],"description":"Where the camera looks from when it frames the result. Default iso."},
			"yaw":{"type":"number","description":"Camera yaw around the result in degrees (overrides view)."},
			"pitch":{"type":"number","description":"Camera pitch in degrees, negative looks down (overrides view)."},
			"distance":{"type":"number","description":"Camera distance from the result center in cm (default: fits everything)."},
			"margin":{"type":"number","description":"Extra room around the result when fitting, default 1.15."},
			"fov":{"type":"number","description":"Horizontal field of view in degrees, default 50."},
			"cameraLocation":{"type":"array","items":{"type":"number"},"description":"[x,y,z] explicit camera position (needs lookAt)."},
			"lookAt":{"type":"array","items":{"type":"number"},"description":"[x,y,z] point to look at; alone it replaces the result center."},
			"cameraActor":{"type":"string","description":"Label of an actor in the open level whose location and rotation (and camera fov) become the view, e.g. the camera of a game_run shot."},
			"inline":{"type":"boolean","description":"Return the image in the reply (default true)."},
			"file":{"type":"string","description":"PNG path; default a new file under Saved/Enginewright/Screenshots."}},
			"required":["class"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FParams P;
			FString Err;
			TArray<FString> Sug;
			P.Class = EW::FindClass(EW::Str(Args, TEXT("class")), Err, &Sug);
			if (!P.Class) { return Bad(TEXT("UNKNOWN_CLASS"), Err, Sug); }
			if (!P.Class->IsChildOf(AActor::StaticClass())) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("%s is not an actor class."), *P.Class->GetName())); }
			if (P.Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("%s is abstract or outdated and cannot be spawned."), *P.Class->GetName())); }
			P.bLevel = EW::Str(Args, TEXT("world")) == TEXT("level");
			bool bHas = false;
			FVector V;
			if (!ReadVec3(Args, TEXT("location"), V, bHas, Err)) { return Bad(TEXT("INVALID_ARGUMENT"), Err); }
			if (bHas) { P.Location = V; }
			if (!ReadVec3(Args, TEXT("rotation"), V, bHas, Err)) { return Bad(TEXT("INVALID_ARGUMENT"), Err); }
			if (bHas) { P.Rotation = FRotator(V.X, V.Y, V.Z); }
			if (!ReadVec3(Args, TEXT("cameraLocation"), P.CamLoc, P.bCamLoc, Err)) { return Bad(TEXT("INVALID_ARGUMENT"), Err); }
			if (!ReadVec3(Args, TEXT("lookAt"), P.LookAt, P.bLookAt, Err)) { return Bad(TEXT("INVALID_ARGUMENT"), Err); }
			if (P.bCamLoc && !P.bLookAt) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("cameraLocation needs lookAt.")); }
			const TSharedPtr<FJsonObject>* Props = nullptr;
			if (Args->TryGetObjectField(TEXT("properties"), Props)) { P.Props = *Props; }
			P.bBeginPlay = EW::Bool(Args, TEXT("beginPlay"), true);
			P.Ticks = (int32)EW::Num(Args, TEXT("ticks"), 3);
			if (P.Ticks < 0 || P.Ticks > 60) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("ticks must be 0 to 60.")); }
			P.bGround = EW::Bool(Args, TEXT("ground"), false);
			P.Lighting = EW::Str(Args, TEXT("lighting")).IsEmpty() ? TEXT("default") : EW::Str(Args, TEXT("lighting"));
			P.Width = (int32)EW::Num(Args, TEXT("width"), 1280);
			P.Height = (int32)EW::Num(Args, TEXT("height"), 720);
			if (P.Width < 16 || P.Height < 16 || P.Width > 4096 || P.Height > 4096) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("width and height must be 16 to 4096.")); }
			if (!EW::Str(Args, TEXT("view")).IsEmpty()) { P.View = EW::Str(Args, TEXT("view")); }
			P.Fov = EW::Num(Args, TEXT("fov"), 50);
			if (P.Fov < 5 || P.Fov > 150) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("fov must be 5 to 150 degrees.")); }
			P.Margin = EW::Num(Args, TEXT("margin"), 1.15);
			if (P.Margin < 0.5 || P.Margin > 10) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("margin must be 0.5 to 10.")); }
			if ((P.bYaw = Args->HasField(TEXT("yaw")))) { P.Yaw = EW::Num(Args, TEXT("yaw"), 0); }
			if ((P.bPitch = Args->HasField(TEXT("pitch")))) { P.Pitch = EW::Num(Args, TEXT("pitch"), 0); }
			if ((P.bDistance = Args->HasField(TEXT("distance")))) { P.Distance = EW::Num(Args, TEXT("distance"), 0); if (P.Distance <= 0) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("distance must be above 0.")); } }
			if ((P.bExposure = Args->HasField(TEXT("exposure")))) { P.Exposure = EW::Num(Args, TEXT("exposure"), 0); }

			UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			const FString CamActorKey = EW::Str(Args, TEXT("cameraActor"));
			float CamActorFov = -1.f;
			if (!CamActorKey.IsEmpty())
			{
				if (!EditorWorld) { return Bad(TEXT("NO_LEVEL"), TEXT("No editor level is open.")); }
				TArray<FString> Labels;
				for (TActorIterator<AActor> It(EditorWorld); It; ++It)
				{
					Labels.Add(It->GetActorLabel());
					if (!P.CameraActor && (It->GetActorLabel() == CamActorKey || It->GetName() == CamActorKey)) { P.CameraActor = *It; }
				}
				if (!P.CameraActor) { return Bad(TEXT("ACTOR_NOT_FOUND"), FString::Printf(TEXT("cameraActor: no actor '%s' in the open level."), *CamActorKey), EW::Suggest(CamActorKey, Labels)); }
				if (UCameraComponent* Cam = P.CameraActor->FindComponentByClass<UCameraComponent>()) { CamActorFov = Cam->FieldOfView; }
			}
			if (P.bLevel && !EditorWorld) { return Bad(TEXT("NO_LEVEL"), TEXT("world level needs an open editor level.")); }

			// ---- the world
			TUniquePtr<FPreviewScene> Scene;
			UWorld* World = nullptr;
			if (P.bLevel) { World = EditorWorld; }
			else
			{
				FPreviewScene::ConstructionValues CV;
				CV.SetCreatePhysicsScene(false).SetTransactional(false).SetEditor(false);
				if (P.Lighting == TEXT("none")) { CV.SetLightBrightness(0.f).SetSkyBrightness(0.f); }
				else { CV.SetLightBrightness(10.f).SetSkyBrightness(1.f); }
				Scene = MakeUnique<FPreviewScene>(CV);
				World = Scene->GetWorld();
			}
			if (!World) { return Bad(TEXT("NO_LEVEL"), TEXT("Could not create a preview world.")); }

			UPackage* LevelPackage = (P.bLevel && World->PersistentLevel) ? World->PersistentLevel->GetOutermost() : nullptr;
			const bool bWasDirty = LevelPackage && LevelPackage->IsDirty();

			TSet<AActor*> Existing;
			for (TActorIterator<AActor> It(World); It; ++It) { Existing.Add(*It); }
			TArray<AActor*> NewActors;
			FEWResult SpawnErr;
			AActor* Main = SpawnPreview(World, P, NewActors, SpawnErr);
			if (!Main) { return SpawnErr; }
			const int32 SpawnedByClass = NewActors.Num();

			for (int32 i = 0; i < P.Ticks && !P.bLevel; ++i) { World->Tick(LEVELTICK_All, 1.f / 30.f); }
			// Anything created while ticking (timers) also counts.
			if (!P.bLevel)
			{
				for (TActorIterator<AActor> It(World); It; ++It) { if (IsValid(*It) && !Existing.Contains(*It) && !NewActors.Contains(*It)) { NewActors.Add(*It); } }
			}

			const FBox Box = VisibleBounds(NewActors);
			FVector Center = Box.IsValid ? Box.GetCenter() : P.Location;
			const double Radius = Box.IsValid ? FMath::Max(1.0, Box.GetExtent().Size()) : 100.0;
			FEWResult Out = FEWResult::Ok();
			if (!Box.IsValid) { Out.Warnings.Add(TEXT("Nothing visible was built (no mesh or other drawn component); the image will be empty. Check beginPlay and the class.")); }

			if (P.bGround && !P.bLevel)
			{
				FActorSpawnParameters GP;
				GP.ObjectFlags = RF_Transient;
				if (AStaticMeshActor* G = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform(FRotator::ZeroRotator, FVector(Center.X, Center.Y, Box.IsValid ? Box.Min.Z : 0.0), FVector(Radius / 20.0, Radius / 20.0, 1.0)), GP))
				{
					G->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
					G->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")));
					NewActors.Add(G);
				}
			}

			// ---- the camera
			double Fov = P.Fov;
			FVector CamLoc;
			FRotator CamRot;
			if (P.CameraActor)
			{
				CamLoc = P.CameraActor->GetActorLocation();
				CamRot = P.CameraActor->GetActorRotation();
				if (CamActorFov > 0 && !Args->HasField(TEXT("fov"))) { Fov = CamActorFov; }
			}
			else if (P.bCamLoc)
			{
				CamLoc = P.CamLoc;
				CamRot = (P.LookAt - P.CamLoc).Rotation();
			}
			else
			{
				if (P.bLookAt) { Center = P.LookAt; }
				double Yaw = 135, Pitch = -28;
				if (P.View == TEXT("front")) { Yaw = 180; Pitch = -8; }
				else if (P.View == TEXT("back")) { Yaw = 0; Pitch = -8; }
				else if (P.View == TEXT("left")) { Yaw = 90; Pitch = -8; }
				else if (P.View == TEXT("right")) { Yaw = -90; Pitch = -8; }
				else if (P.View == TEXT("top")) { Yaw = 180; Pitch = -89; }
				if (P.bYaw) { Yaw = P.Yaw; }
				if (P.bPitch) { Pitch = P.Pitch; }
				const double Aspect = (double)P.Width / P.Height;
				const double HalfH = FMath::Atan(FMath::Tan(FMath::DegreesToRadians(Fov) * 0.5) / Aspect);
				const double HalfMin = FMath::Min(FMath::DegreesToRadians(Fov) * 0.5, HalfH);
				const double Dist = P.bDistance ? P.Distance : P.Margin * Radius / FMath::Sin(HalfMin);
				// The camera sits on the side the view names; "front" looks at the actor's +X face from +X.
				const FRotator Look(Pitch, Yaw, 0);
				CamLoc = Center - Look.Vector() * Dist;
				CamRot = Look;
			}

			// ---- light: the sun shines the way the camera looks (so the visible sides are lit), a weak fill from the other side
			if (Scene.IsValid() && P.Lighting != TEXT("none"))
			{
				Scene->SetLightDirection(FRotator(-48, CamRot.Yaw + 30, 0));
				UDirectionalLightComponent* Fill = NewObject<UDirectionalLightComponent>(GetTransientPackage(), NAME_None, RF_Transient);
				Fill->Intensity = 2.5f;
				Fill->bTransmission = false;
				Fill->CastShadows = false;
				Scene->AddComponent(Fill, FTransform(FRotator(-20, CamRot.Yaw + 180 - 35, 0)));
			}

			// ---- capture
			UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
			RT->ClearColor = FLinearColor::Black;
			RT->InitCustomFormat(P.Width, P.Height, PF_B8G8R8A8, /*bInForceLinearGamma*/ false);
			RT->UpdateResourceImmediate(true);
			USceneCaptureComponent2D* Cap = NewObject<USceneCaptureComponent2D>(GetTransientPackage());
			Cap->TextureTarget = RT;
			Cap->CaptureSource = SCS_FinalColorLDR;
			Cap->FOVAngle = (float)Fov;
			Cap->bCaptureEveryFrame = false;
			Cap->bCaptureOnMovement = false;
			Cap->bAlwaysPersistRenderingState = true;
			if (!P.bLevel)
			{
				// A private world has no level exposure: pin it (EV100, like the editor's fixed exposure) so the image does not depend on a slow eye adaptation.
				Cap->PostProcessBlendWeight = 1.f;
				FPostProcessSettings& PP = Cap->PostProcessSettings;
				PP.bOverride_AutoExposureMethod = true;
				PP.AutoExposureMethod = AEM_Histogram;
				PP.bOverride_AutoExposureMinBrightness = true;
				PP.bOverride_AutoExposureMaxBrightness = true;
				PP.AutoExposureMinBrightness = (float)P.Exposure;
				PP.AutoExposureMaxBrightness = (float)P.Exposure;
			}
			else if (P.bExposure)
			{
				Cap->PostProcessBlendWeight = 1.f;
				FPostProcessSettings& PP = Cap->PostProcessSettings;
				PP.bOverride_AutoExposureBias = true;
				PP.AutoExposureBias = (float)P.Exposure;
			}
			Cap->SetWorldLocationAndRotation(CamLoc, CamRot);
			Cap->RegisterComponentWithWorld(World);
			FlushRenderingCommands();
			if (GShaderCompilingManager) { GShaderCompilingManager->FinishAllCompilation(); }
			Cap->UpdateComponentToWorld();
			Cap->CaptureScene();
			FlushRenderingCommands();
			if (GShaderCompilingManager) { GShaderCompilingManager->FinishAllCompilation(); }
			Cap->CaptureScene(); // second pass: shadows and streamed textures settled
			FlushRenderingCommands();
			TArray<FColor> Pixels;
			FTextureRenderTargetResource* Res = RT->GameThread_GetRenderTargetResource();
			const bool bRead = Res && Res->ReadPixels(Pixels);
			Cap->UnregisterComponent();
			Cap->MarkAsGarbage();
			RT->MarkAsGarbage();

			// ---- clean up: nothing stays behind
			TArray<AActor*> Doomed = NewActors;
			DestroyAll(World, Doomed);
			Scene.Reset();
			if (LevelPackage && !bWasDirty) { LevelPackage->SetDirtyFlag(false); }

			if (!bRead || Pixels.Num() != P.Width * P.Height) { return Bad(TEXT("CAPTURE_FAILED"), TEXT("Could not read the rendered pixels.")); }
			int64 NonBlack = 0;
			double Luma = 0;
			for (FColor& C : Pixels) { C.A = 255; if (C.R > 8 || C.G > 8 || C.B > 8) { ++NonBlack; } Luma += (0.2126 * C.R + 0.7152 * C.G + 0.0722 * C.B) / 255.0; }
			TArray64<uint8> Png;
			if (!EW::EncodePng(Pixels, P.Width, P.Height, Png)) { return Bad(TEXT("CAPTURE_FAILED"), TEXT("PNG encoding failed.")); }
			FString File = EW::Str(Args, TEXT("file"));
			if (File.IsEmpty())
			{
				FString Name = P.Class->GetName();
				Name.RemoveFromEnd(TEXT("_C"));
				File = FPaths::ProjectSavedDir() / TEXT("Enginewright/Screenshots") / FString::Printf(TEXT("preview_%s_%s.png"), *Name, *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S_%s")));
			}
			File = FPaths::ConvertRelativePathToFull(File);
			if (!FFileHelper::SaveArrayToFile(Png, *File)) { return Bad(TEXT("SAVE_FAILED"), FString::Printf(TEXT("Could not write %s."), *File)); }

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("file"), File);
			Data->SetStringField(TEXT("class"), P.Class->GetPathName());
			Data->SetStringField(TEXT("world"), P.bLevel ? TEXT("level") : TEXT("isolated"));
			Data->SetNumberField(TEXT("width"), P.Width);
			Data->SetNumberField(TEXT("height"), P.Height);
			Data->SetNumberField(TEXT("actorsBuilt"), SpawnedByClass);
			Data->SetNumberField(TEXT("actorsTotal"), NewActors.Num());
			Data->SetBoolField(TEXT("beganPlay"), P.bBeginPlay);
			Data->SetArrayField(TEXT("boundsCenter"), Vec(Box.IsValid ? Box.GetCenter() : FVector::ZeroVector));
			Data->SetArrayField(TEXT("boundsSize"), Vec(Box.IsValid ? Box.GetSize() : FVector::ZeroVector));
			Data->SetArrayField(TEXT("cameraLocation"), Vec(CamLoc));
			Data->SetArrayField(TEXT("cameraRotation"), Vec(FVector(CamRot.Pitch, CamRot.Yaw, CamRot.Roll)));
			Data->SetNumberField(TEXT("fov"), Fov);
			Data->SetNumberField(TEXT("drawnPercent"), FMath::RoundToDouble(10000.0 * NonBlack / Pixels.Num()) / 100);
			Data->SetNumberField(TEXT("meanLuma"), FMath::RoundToDouble(Luma / Pixels.Num() * 1000.0) / 1000.0);
			Data->SetBoolField(TEXT("cleanedUp"), true);
			Out.Data = Data;
			if (NonBlack == 0) { Out.Warnings.Add(TEXT("The image is black. Check lighting (isolated needs lighting default or lights in the class), the camera and the exposure.")); }
			if (EW::Bool(Args, TEXT("inline"), true)) { Out.Images.Emplace(TEXT("image/png"), FBase64::Encode(Png.GetData(), Png.Num())); }
			return Out;
		});
}
