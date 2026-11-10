#include "EnginewrightTool.h"
#include "EWCommon.h"

#include "Camera/PlayerCameraManager.h"
#include "Containers/Ticker.h"
#include "GameFramework/PawnMovementComponent.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "LevelEditorViewport.h"
#include "Misc/Base64.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UnrealClient.h"
#include "Widgets/SViewport.h"

namespace
{
	const TCHAR* NetModeName(ENetMode M)
	{
		switch (M)
		{
		case NM_Standalone: return TEXT("standalone");
		case NM_DedicatedServer: return TEXT("dedicatedServer");
		case NM_ListenServer: return TEXT("listenServer");
		case NM_Client: return TEXT("client");
		default: return TEXT("unknown");
		}
	}

	TArray<TSharedPtr<FJsonValue>> Vec(const FVector& V)
	{
		return { MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.X * 10) / 10), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Y * 10) / 10), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Z * 10) / 10) };
	}

	int32 LocalPlayerCount()
	{
		int32 N = 0;
		for (UWorld* W : EW::PieWorlds())
		{
			for (FConstPlayerControllerIterator It = W->GetPlayerControllerIterator(); It; ++It)
			{
				if (It->Get() && It->Get()->IsLocalController() && It->Get()->GetPawn()) { ++N; }
			}
		}
		return N;
	}

	TSharedRef<FJsonObject> PieState()
	{
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("playing"), EW::IsPlaying());
		TArray<TSharedPtr<FJsonValue>> Worlds;
		int32 PlayerIndex = 0;
		for (UWorld* W : EW::PieWorlds())
		{
			TSharedRef<FJsonObject> WJ = MakeShared<FJsonObject>();
			WJ->SetStringField(TEXT("netMode"), NetModeName(W->GetNetMode()));
			WJ->SetStringField(TEXT("map"), W->GetMapName());
			WJ->SetNumberField(TEXT("timeSeconds"), W->GetTimeSeconds());
			TArray<TSharedPtr<FJsonValue>> Players;
			for (FConstPlayerControllerIterator It = W->GetPlayerControllerIterator(); It; ++It)
			{
				APlayerController* PC = It->Get();
				if (!PC) { continue; }
				TSharedRef<FJsonObject> PJ = MakeShared<FJsonObject>();
				PJ->SetBoolField(TEXT("local"), PC->IsLocalController());
				if (PC->IsLocalController()) { PJ->SetNumberField(TEXT("player"), PlayerIndex++); }
				PJ->SetStringField(TEXT("controller"), PC->GetName());
				if (APawn* P = PC->GetPawn())
				{
					PJ->SetStringField(TEXT("pawn"), P->GetClass()->GetName());
					PJ->SetArrayField(TEXT("location"), Vec(P->GetActorLocation()));
					PJ->SetArrayField(TEXT("rotation"), Vec(FVector(P->GetActorRotation().Pitch, P->GetActorRotation().Yaw, P->GetActorRotation().Roll)));
				}
				if (PC->PlayerCameraManager)
				{
					PJ->SetArrayField(TEXT("cameraLocation"), Vec(PC->PlayerCameraManager->GetCameraLocation()));
					const FRotator CR = PC->PlayerCameraManager->GetCameraRotation();
					PJ->SetArrayField(TEXT("cameraRotation"), Vec(FVector(CR.Pitch, CR.Yaw, CR.Roll)));
					PJ->SetNumberField(TEXT("fov"), PC->PlayerCameraManager->GetFOVAngle());
				}
				Players.Add(MakeShared<FJsonValueObject>(PJ));
			}
			WJ->SetArrayField(TEXT("players"), Players);
			Worlds.Add(MakeShared<FJsonValueObject>(WJ));
		}
		Data->SetArrayField(TEXT("worlds"), Worlds);
		Data->SetNumberField(TEXT("localPlayersWithPawn"), LocalPlayerCount());
		return Data;
	}

	/** Reads a viewport into an image result; downsizes to MaxWidth. */
	/** Draws and reads a viewport, downsized to MaxWidth. */
	/** With UIWidget (the game viewport widget) the capture goes through Slate and includes the UI drawn over the
	 *  scene (UMG, CommonUI); bOutUI tells whether that worked. Without it, or when it fails, only the scene is read. */
	bool Grab(FViewport* V, int32 MaxWidth, TArray<FColor>& Pixels, int32& W, int32& H, FEWResult& OutErr,
		TSharedPtr<SWidget> UIWidget = nullptr, bool* bOutUI = nullptr)
	{
		if (bOutUI) { *bOutUI = false; }
		if (!V) { OutErr = FEWResult::Error(TEXT("NO_VIEWPORT"), TEXT("No viewport to capture.")); return false; }
		const FIntPoint Size = V->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0) { OutErr = FEWResult::Error(TEXT("NO_VIEWPORT"), TEXT("The viewport has no size (window minimized or hidden?).")); return false; }
		FIntVector UISize(0, 0, 0);
		if (UIWidget.IsValid() && FSlateApplication::IsInitialized()
			&& FSlateApplication::Get().TakeScreenshot(UIWidget.ToSharedRef(), Pixels, UISize)
			&& UISize.X > 0 && UISize.Y > 0 && Pixels.Num() == UISize.X * UISize.Y)
		{
			W = UISize.X; H = UISize.Y;
			if (bOutUI) { *bOutUI = true; }
		}
		else
		{
			V->Draw();
			if (!V->ReadPixels(Pixels, FReadSurfaceDataFlags(), FIntRect(0, 0, Size.X, Size.Y)) || Pixels.Num() != Size.X * Size.Y)
			{
				OutErr = FEWResult::Error(TEXT("CAPTURE_FAILED"), TEXT("Could not read the viewport pixels."));
				return false;
			}
			W = Size.X; H = Size.Y;
		}
		if (MaxWidth > 0 && W > MaxWidth)
		{
			const int32 NW = MaxWidth, NH = FMath::Max(1, FMath::RoundToInt((double)H * NW / W));
			TArray<FColor> Small;
			FImageUtils::ImageResize(W, H, Pixels, NW, NH, Small, /*bLinearSpace*/ false, /*bForceOpaqueOutput*/ true);
			Pixels = MoveTemp(Small);
			W = NW; H = NH;
		}
		for (FColor& C : Pixels) { C.A = 255; }
		return true;
	}

	double MeanLuma(const TArray<FColor>& Pixels)
	{
		double L = 0;
		for (const FColor& C : Pixels) { L += 0.299 * C.R + 0.587 * C.G + 0.114 * C.B; }
		return L / (255.0 * FMath::Max(1, Pixels.Num()));
	}

	FEWResult Capture(FViewport* V, const FString& Source, int32 MaxWidth, bool bInline, const FString& SaveTo, TSharedPtr<SWidget> UIWidget = nullptr)
	{
		if (!V) { return FEWResult::Error(TEXT("NO_VIEWPORT"), FString::Printf(TEXT("No %s viewport to capture."), *Source)); }
		const FIntPoint Size = V->GetSizeXY();
		TArray<FColor> Pixels;
		int32 W = 0, H = 0;
		FEWResult GrabErr;
		bool bUI = false;
		if (!Grab(V, MaxWidth, Pixels, W, H, GrabErr, UIWidget, &bUI)) { return GrabErr; }
		const double Luma = MeanLuma(Pixels);
		TArray64<uint8> Png;
		if (!EW::EncodePng(Pixels, W, H, Png)) { return FEWResult::Error(TEXT("CAPTURE_FAILED"), TEXT("PNG encoding failed.")); }
		FString File = SaveTo;
		if (File.IsEmpty())
		{
			File = FPaths::ProjectSavedDir() / TEXT("Enginewright/Screenshots") / FString::Printf(TEXT("%s_%s.png"), *Source, *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S_%s")));
		}
		File = FPaths::ConvertRelativePathToFull(File);
		if (!FFileHelper::SaveArrayToFile(Png, *File)) { return FEWResult::Error(TEXT("SAVE_FAILED"), FString::Printf(TEXT("Could not write %s."), *File)); }

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("source"), Source);
		Data->SetStringField(TEXT("file"), File);
		Data->SetNumberField(TEXT("width"), W);
		Data->SetNumberField(TEXT("height"), H);
		Data->SetNumberField(TEXT("viewportWidth"), Size.X);
		Data->SetNumberField(TEXT("viewportHeight"), Size.Y);
		Data->SetNumberField(TEXT("meanBrightness"), FMath::RoundToDouble(Luma * 1000) / 1000);
		if (Source == TEXT("game")) { Data->SetBoolField(TEXT("ui"), bUI); }
		FEWResult Res = FEWResult::Ok(Data);
		if (UIWidget.IsValid() && !bUI) { Res.Warnings.Add(TEXT("The UI could not be captured through Slate; the image shows the scene only.")); }
		if (Luma < 0.03) { Res.Warnings.Add(TEXT("The image is almost black: the scene may be unlit, or the window was not drawn.")); }
		if (bInline) { Res.Images.Emplace(TEXT("image/png"), FBase64::Encode(Png.GetData(), Png.Num())); }
		return Res;
	}

	bool ParseKey(const FString& Name, FKey& Out, TArray<FString>& OutSuggest)
	{
		Out = FKey(FName(*Name));
		if (Out.IsValid()) { return true; }
		TArray<FKey> All;
		EKeys::GetAllKeys(All);
		TArray<FString> Names;
		for (const FKey& K : All) { Names.Add(K.GetFName().ToString()); }
		for (const FString& N : Names)
		{
			if (N.Equals(Name, ESearchCase::IgnoreCase)) { Out = FKey(FName(*N)); return true; }
		}
		OutSuggest = EW::Suggest(Name, Names);
		return false;
	}

	FInputActionValue MakeActionValue(const TSharedPtr<FJsonValue>& V, EInputActionValueType Type, FString& OutError)
	{
		if (!V.IsValid()) { return Type == EInputActionValueType::Boolean ? FInputActionValue(true) : FInputActionValue(1.0f); }
		switch (V->Type)
		{
		case EJson::Boolean: return FInputActionValue(V->AsBool());
		case EJson::Number: return FInputActionValue((float)V->AsNumber());
		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>& A = V->AsArray();
			if (A.Num() == 2) { return FInputActionValue(FVector2D(A[0]->AsNumber(), A[1]->AsNumber())); }
			if (A.Num() == 3) { return FInputActionValue(FVector(A[0]->AsNumber(), A[1]->AsNumber(), A[2]->AsNumber())); }
		}
		default: break;
		}
		OutError = TEXT("value must be true/false, a number, [x, y] or [x, y, z].");
		return FInputActionValue();
	}
}

void RegisterPieTools(FEWRegistry& R)
{
	R.RegisterAsync(TEXT("pie_start"),
		TEXT("Start Play In Editor: players (1-4), netMode standalone / listen (player 0 hosts) / client (dedicated server + clients). By default waits until every player has a pawn (timeout seconds). Multi-player sessions open extra windows."),
		TEXT(R"({"type":"object","properties":{
			"simulate":{"type":"boolean","description":"Simulate instead of play (no player)."},
			"players":{"type":"integer","description":"Number of players, default 1."},
			"netMode":{"type":"string","enum":["standalone","listen","client"],"description":"Default standalone (listen when players > 1)."},
			"wait":{"type":"boolean","description":"Wait until all players have pawns (default true)."},
			"timeout":{"type":"number","description":"Seconds to wait, default 60."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args, FEWDone Done)
		{
			if (EW::IsPlaying()) { Done(FEWResult::Error(TEXT("PIE_RUNNING"), TEXT("Play In Editor already runs (pie_stop first)."))); return; }
			const bool bSimulate = EW::Bool(Args, TEXT("simulate"), false);
			const int32 Players = (int32)EW::Num(Args, TEXT("players"), 1);
			if (Players < 1 || Players > 4) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("players must be 1 to 4."))); return; }
			FString Mode = EW::Str(Args, TEXT("netMode")).ToLower();
			if (Mode.IsEmpty()) { Mode = Players > 1 ? TEXT("listen") : TEXT("standalone"); }
			if (bSimulate && (Players > 1 || Mode != TEXT("standalone"))) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("simulate supports one standalone session only."))); return; }
			if (Mode == TEXT("standalone") && Players > 1) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("Several players need netMode listen or client."))); return; }

			ULevelEditorPlaySettings* PS = DuplicateObject(GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage());
			PS->SetPlayNetMode(Mode == TEXT("listen") ? EPlayNetMode::PIE_ListenServer : Mode == TEXT("client") ? EPlayNetMode::PIE_Client : EPlayNetMode::PIE_Standalone);
			PS->SetPlayNumberOfClients(Players);
			PS->SetRunUnderOneProcess(true);
			FRequestPlaySessionParams Params;
			Params.WorldType = bSimulate ? EPlaySessionWorldType::SimulateInEditor : EPlaySessionWorldType::PlayInEditor;
			Params.EditorPlaySettings = PS;
			GEditor->RequestPlaySession(Params);

			if (!EW::Bool(Args, TEXT("wait"), true))
			{
				Done(FEWResult::Ok(nullptr, TEXT("Play In Editor requested; poll pie_state.")));
				return;
			}
			const float Timeout = (float)EW::Num(Args, TEXT("timeout"), 60);
			const int32 Want = bSimulate ? 0 : Players;
			EW::PollUntil(Timeout,
				[Done, Want, bSimulate]() -> bool
				{
					if (!EW::IsPlaying() || EW::PieWorlds().Num() == 0) { return false; }
					if (!bSimulate && LocalPlayerCount() < Want) { return false; }
					FEWResult Res = FEWResult::Ok(PieState(), TEXT("Play In Editor is running."));
					Done(Res);
					return true;
				},
				[Done, Want]()
				{
					FEWResult Res = FEWResult::Error(TEXT("TIMEOUT"), FString::Printf(TEXT("Play In Editor did not reach %d player(s) with a pawn in time. See data and log_read."), Want));
					Res.Data = PieState();
					Done(Res);
				});
		});

	R.RegisterAsync(TEXT("pie_stop"),
		TEXT("Stop Play In Editor and wait until it has ended."),
		TEXT(R"({"type":"object","properties":{}})"),
		false,
		[](const TSharedPtr<FJsonObject>&, FEWDone Done)
		{
			if (!EW::IsPlaying()) { Done(FEWResult::Ok(nullptr, TEXT("Play In Editor was not running."))); return; }
			GEditor->RequestEndPlayMap();
			EW::PollUntil(30.f,
				[Done]() -> bool { if (EW::IsPlaying()) { return false; } Done(FEWResult::Ok(nullptr, TEXT("Stopped."))); return true; },
				[Done]() { Done(FEWResult::Error(TEXT("TIMEOUT"), TEXT("Play In Editor did not stop within 30 seconds."))); });
		});

	R.Register(TEXT("pie_state"),
		TEXT("Play In Editor state: each world (server/client, net mode, map, time) and its players (pawn, location, camera location, rotation and FOV)."),
		TEXT(R"({"type":"object","properties":{}})"),
		false,
		[](const TSharedPtr<FJsonObject>&) -> FEWResult { return FEWResult::Ok(PieState()); });

	R.RegisterAsync(TEXT("pie_input"),
		TEXT("Send synthetic input to a Play In Editor player: an Enhanced Input action (with a value) or a key (e.g. SpaceBar, W, LeftMouseButton, Gamepad_FaceButton_Bottom). Holds it for seconds, then releases, and returns after the release."),
		TEXT(R"({"type":"object","properties":{
			"player":{"type":"integer","description":"Local player index (0 = first / host). Default 0."},
			"action":{"type":"string","description":"Input Action asset path, e.g. /Game/Input/IA_Jump."},
			"key":{"type":"string","description":"Key name, e.g. SpaceBar."},
			"value":{"type":["boolean","number","array"],"description":"Action value: true, 0.5, [x,y] or [x,y,z]. Default true / 1."},
			"seconds":{"type":"number","description":"Hold time; default 0.1. Max 30."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args, FEWDone Done)
		{
			FString Err;
			APlayerController* PC = EW::PiePlayer((int32)EW::Num(Args, TEXT("player"), 0), Err);
			if (!PC) { Done(FEWResult::Error(TEXT("NO_PLAYER"), Err)); return; }
			const FString ActionPath = EW::Str(Args, TEXT("action"));
			const FString KeyName = EW::Str(Args, TEXT("key"));
			if (ActionPath.IsEmpty() == KeyName.IsEmpty()) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("Pass exactly one of action or key."))); return; }
			const float Seconds = FMath::Clamp((float)EW::Num(Args, TEXT("seconds"), 0.1), 0.f, 30.f);
			TWeakObjectPtr<APlayerController> WeakPC = PC;
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("seconds"), Seconds);

			if (!ActionPath.IsEmpty())
			{
				UInputAction* Action = EW::LoadAs<UInputAction>(ActionPath, Err);
				if (!Action) { Done(FEWResult::Error(TEXT("ACTION_NOT_FOUND"), Err)); return; }
				UEnhancedInputLocalPlayerSubsystem* Sub = PC->GetLocalPlayer() ? PC->GetLocalPlayer()->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
				if (!Sub) { Done(FEWResult::Error(TEXT("NO_ENHANCED_INPUT"), TEXT("This player has no Enhanced Input subsystem."))); return; }
				const FInputActionValue Value = MakeActionValue(Args->TryGetField(TEXT("value")), Action->ValueType, Err);
				if (!Err.IsEmpty()) { Done(FEWResult::Error(TEXT("INVALID_VALUE"), Err)); return; }
				Sub->StartContinuousInputInjectionForAction(Action, Value, {}, {});
				Data->SetStringField(TEXT("action"), Action->GetPathName());
				Data->SetStringField(TEXT("value"), Value.ToString());
				TWeakObjectPtr<UInputAction> WeakAction = Action;
				TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> WeakSub = Sub;
				EW::PollUntil(Seconds, []() { return false; }, [Done, Data, WeakAction, WeakSub]()
				{
					if (WeakSub.IsValid() && WeakAction.IsValid()) { WeakSub->StopContinuousInputInjectionForAction(WeakAction.Get()); }
					Done(FEWResult::Ok(Data, TEXT("Action injected and released.")));
				});
				return;
			}

			FKey Key;
			TArray<FString> Sug;
			if (!ParseKey(KeyName, Key, Sug)) { Done(FEWResult::Error(TEXT("UNKNOWN_KEY"), FString::Printf(TEXT("No key '%s'."), *KeyName), Sug)); return; }
			PC->InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Pressed, 1.0f));
			Data->SetStringField(TEXT("key"), Key.GetFName().ToString());
			EW::PollUntil(Seconds, []() { return false; }, [Done, Data, WeakPC, Key]()
			{
				if (WeakPC.IsValid()) { WeakPC->InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Released, 0.0f)); }
				Done(FEWResult::Ok(Data, TEXT("Key pressed and released.")));
			});
		});

	R.RegisterAsync(TEXT("pie_walk_to"),
		TEXT("Walk a Play In Editor player's pawn to a world position, like a player holding the move keys: every frame it adds movement input toward the target, so the game's own movement (speed, collision, animation) does the walking. With a navigation mesh in the level (mode auto, the default) it follows the navmesh path; without one it walks straight. Stops when the pawn is within acceptRadius (ok, reached), when it has not moved for stuckSeconds (STUCK, with the last position and the blocking hint), or after timeout seconds (TIMEOUT). Every reply carries the final location and distance. Use pie_state to read positions and pie_input for single key presses."),
		TEXT(R"({"type":"object","properties":{
			"x":{"type":"number","description":"Target world X."},
			"y":{"type":"number","description":"Target world Y."},
			"z":{"type":"number","description":"Target world Z. Omit to ignore height (distance is then measured on the ground plane)."},
			"player":{"type":"integer","description":"Local player index (0 = first / host). Default 0."},
			"acceptRadius":{"type":"number","description":"Reached when closer than this many units. Default 80."},
			"timeout":{"type":"number","description":"Give up after this many seconds. Default 30, max 300."},
			"stuckSeconds":{"type":"number","description":"STUCK when the pawn moved less than 10 units in this time. Default 2."},
			"mode":{"type":"string","enum":["auto","navmesh","direct"],"description":"auto (default): navmesh path when one exists, else straight; navmesh: fail with NO_NAVMESH when there is no path; direct: straight line, no navmesh."}},
			"required":["x","y"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args, FEWDone Done)
		{
			FString Err;
			APlayerController* PC = EW::PiePlayer((int32)EW::Num(Args, TEXT("player"), 0), Err);
			if (!PC) { Done(FEWResult::Error(TEXT("NO_PLAYER"), Err)); return; }
			APawn* Pawn = PC->GetPawn();
			if (!Pawn) { Done(FEWResult::Error(TEXT("NO_PAWN"), TEXT("This player has no pawn yet (the game may still be loading or spawning).")));  return; }
			if (!Pawn->GetMovementComponent()) { Done(FEWResult::Error(TEXT("NO_MOVEMENT"), FString::Printf(TEXT("%s has no movement component, so movement input does nothing."), *Pawn->GetClass()->GetName()))); return; }
			if (PC->IsMoveInputIgnored()) { Done(FEWResult::Error(TEXT("MOVE_INPUT_IGNORED"), TEXT("The game ignores this player's move input right now (a cutscene, menu or tutorial gate usually sets it). Finish that first; ui_state shows what is on screen."))); return; }
			const FString Mode = Args->HasField(TEXT("mode")) ? EW::Str(Args, TEXT("mode")).ToLower() : TEXT("auto");
			if (Mode != TEXT("auto") && Mode != TEXT("navmesh") && Mode != TEXT("direct")) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("mode must be auto, navmesh or direct."))); return; }
			const float Accept = (float)EW::Num(Args, TEXT("acceptRadius"), 80);
			const float Timeout = (float)EW::Num(Args, TEXT("timeout"), 30);
			const float StuckSeconds = (float)EW::Num(Args, TEXT("stuckSeconds"), 2);
			if (Accept < 1.f || Accept > 100000.f) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("acceptRadius must be 1 to 100000."))); return; }
			if (Timeout <= 0.f || Timeout > 300.f) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("timeout must be above 0 and at most 300 seconds."))); return; }
			if (StuckSeconds < 0.5f || StuckSeconds > 30.f) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("stuckSeconds must be 0.5 to 30."))); return; }
			const bool bHasZ = Args->HasField(TEXT("z"));
			const FVector Start = Pawn->GetActorLocation();
			const FVector Target((float)EW::Num(Args, TEXT("x"), 0), (float)EW::Num(Args, TEXT("y"), 0), bHasZ ? (float)EW::Num(Args, TEXT("z"), 0) : Start.Z);

			// Waypoints: the navmesh path, or the target itself.
			TArray<FVector> Way;
			FString Used = TEXT("direct");
			if (Mode != TEXT("direct"))
			{
				UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(Pawn->GetWorld());
				UNavigationPath* Path = (Nav && Nav->GetDefaultNavDataInstance()) ? Nav->FindPathToLocationSynchronously(Pawn->GetWorld(), Start, Target, Pawn) : nullptr;
				if (Path && Path->IsValid() && Path->PathPoints.Num() > 1)
				{
					for (int32 i = 1; i < Path->PathPoints.Num(); ++i) { Way.Add(Path->PathPoints[i]); }
					Used = TEXT("navmesh");
					if (Path->IsPartial()) { Used = TEXT("navmeshPartial"); }
				}
				else if (Mode == TEXT("navmesh")) { Done(FEWResult::Error(TEXT("NO_NAVMESH"), TEXT("No navigation path to the target: the level has no navmesh, or the target is off it. Use mode auto or direct."))); return; }
			}
			if (Way.Num() == 0) { Way.Add(Target); }

			struct FWalk
			{
				TWeakObjectPtr<APawn> Pawn;
				TWeakObjectPtr<APlayerController> PC;
				TArray<FVector> Way;
				int32 Index = 0;
				FVector Target, Start, SamplePos;
				bool bHasZ = false;
				double T0 = 0, SampleTime = 0, End = 0;
				float Accept = 80, StuckSeconds = 2;
				FString Mode;
			};
			TSharedRef<FWalk> W = MakeShared<FWalk>();
			W->Pawn = Pawn; W->PC = PC; W->Way = Way; W->Target = Target; W->Start = Start; W->SamplePos = Start; W->bHasZ = bHasZ;
			W->T0 = W->SampleTime = FPlatformTime::Seconds(); W->End = W->T0 + Timeout; W->Accept = Accept; W->StuckSeconds = StuckSeconds; W->Mode = Used;

			auto Report = [](const FWalk& Wk, APawn* P, const TCHAR* Status) -> TSharedRef<FJsonObject>
			{
				TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
				const FVector L = P ? P->GetActorLocation() : Wk.SamplePos;
				const FVector Delta = Wk.Target - L;
				D->SetStringField(TEXT("status"), Status);
				D->SetBoolField(TEXT("reached"), FCString::Strcmp(Status, TEXT("reached")) == 0);
				D->SetArrayField(TEXT("location"), Vec(L));
				D->SetArrayField(TEXT("target"), Vec(Wk.Target));
				D->SetNumberField(TEXT("distance"), FMath::RoundToDouble((Wk.bHasZ ? Delta.Size() : Delta.Size2D()) * 10) / 10);
				D->SetNumberField(TEXT("seconds"), FMath::RoundToDouble((FPlatformTime::Seconds() - Wk.T0) * 100) / 100);
				D->SetStringField(TEXT("mode"), Wk.Mode);
				D->SetNumberField(TEXT("waypoints"), Wk.Way.Num());
				return D;
			};

			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([W, Done, Report](float) -> bool
			{
				APawn* P = W->Pawn.Get();
				APlayerController* Ctl = W->PC.Get();
				if (!P || !Ctl || !EW::IsPlaying()) { Done(FEWResult::Error(TEXT("PIE_STOPPED"), TEXT("The pawn or Play In Editor went away while walking."))); return false; }
				const FVector L = P->GetActorLocation();
				const double Now = FPlatformTime::Seconds();
				const FVector ToTarget = W->Target - L;
				if ((W->bHasZ ? ToTarget.Size() : ToTarget.Size2D()) <= W->Accept)
				{
					Done(FEWResult::Ok(Report(*W, P, TEXT("reached")), TEXT("Reached the target.")));
					return false;
				}
				// Next waypoint: skip the ones already close (the last one uses the accept radius above).
				while (W->Index < W->Way.Num() - 1 && (W->Way[W->Index] - L).Size2D() < FMath::Max(60.f, W->Accept * 0.5f)) { ++W->Index; }
				FVector Dir = W->Way[W->Index] - L;
				Dir.Z = 0;
				if (!Dir.IsNearlyZero()) { P->AddMovementInput(Dir.GetSafeNormal(), 1.0f); }
				if ((L - W->SamplePos).Size() > 10.f) { W->SamplePos = L; W->SampleTime = Now; }
				else if (Now - W->SampleTime > W->StuckSeconds)
				{
					FEWResult Res = FEWResult::Error(TEXT("STUCK"), FString::Printf(TEXT("The pawn did not move for %.1f s at %.0f, %.0f, %.0f (something blocks it, or the game does not use movement input). Look at screenshot, ui_state (a menu may own the input) and pie_state."), W->StuckSeconds, L.X, L.Y, L.Z));
					Res.Data = Report(*W, P, TEXT("stuck"));
					Done(Res);
					return false;
				}
				if (Now > W->End)
				{
					FEWResult Res = FEWResult::Error(TEXT("TIMEOUT"), TEXT("The pawn did not reach the target in time. data has its last position."));
					Res.Data = Report(*W, P, TEXT("timeout"));
					Done(Res);
					return false;
				}
				return true;
			}));
		});

	R.Register(TEXT("screenshot"),
		TEXT("Capture what a Play In Editor player sees, UI included (source game, the default while playing), or the level editor viewport (source editor). Saves a PNG under Saved/Enginewright/Screenshots and returns it as an image, with its mean brightness (warns when nearly black)."),
		TEXT(R"({"type":"object","properties":{
			"source":{"type":"string","enum":["game","editor"]},
			"player":{"type":"integer","description":"Local player for source game; default 0."},
			"ui":{"type":"boolean","description":"Source game: include the UI drawn over the scene (UMG, CommonUI, HUD widgets). Default true; false = the 3D scene only. data.ui tells what the image shows."},
			"maxWidth":{"type":"integer","description":"Downsize to this width; default 1280, 0 = full size."},
			"inline":{"type":"boolean","description":"Return the image in the reply (default true)."},
			"file":{"type":"string","description":"PNG path to write; default a new file under Saved/Enginewright/Screenshots."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Source = EW::Str(Args, TEXT("source")).ToLower();
			if (Source.IsEmpty()) { Source = EW::IsPlaying() ? TEXT("game") : TEXT("editor"); }
			const int32 MaxWidth = (int32)EW::Num(Args, TEXT("maxWidth"), 1280);
			const bool bInline = EW::Bool(Args, TEXT("inline"), true);
			const FString File = EW::Str(Args, TEXT("file"));
			FViewport* V = nullptr;
			TSharedPtr<SWidget> UIWidget;
			TSharedPtr<FJsonObject> Camera;
			if (Source == TEXT("game"))
			{
				FString Err;
				APlayerController* PC = EW::PiePlayer((int32)EW::Num(Args, TEXT("player"), 0), Err);
				if (!PC) { return FEWResult::Error(TEXT("NO_PLAYER"), Err); }
				ULocalPlayer* LP = PC->GetLocalPlayer();
				V = LP && LP->ViewportClient ? LP->ViewportClient->Viewport : nullptr;
				if (V && EW::Bool(Args, TEXT("ui"), true)) { UIWidget = LP->ViewportClient->GetGameViewportWidget(); }
				if (PC->PlayerCameraManager)
				{
					Camera = MakeShared<FJsonObject>();
					Camera->SetArrayField(TEXT("location"), Vec(PC->PlayerCameraManager->GetCameraLocation()));
					const FRotator CR = PC->PlayerCameraManager->GetCameraRotation();
					Camera->SetArrayField(TEXT("rotation"), Vec(FVector(CR.Pitch, CR.Yaw, CR.Roll)));
					Camera->SetNumberField(TEXT("fov"), PC->PlayerCameraManager->GetFOVAngle());
				}
			}
			else
			{
				V = GCurrentLevelEditingViewportClient ? GCurrentLevelEditingViewportClient->Viewport : nullptr;
				if (GCurrentLevelEditingViewportClient)
				{
					Camera = MakeShared<FJsonObject>();
					Camera->SetArrayField(TEXT("location"), Vec(GCurrentLevelEditingViewportClient->GetViewLocation()));
					const FRotator CR = GCurrentLevelEditingViewportClient->GetViewRotation();
					Camera->SetArrayField(TEXT("rotation"), Vec(FVector(CR.Pitch, CR.Yaw, CR.Roll)));
					Camera->SetNumberField(TEXT("fov"), GCurrentLevelEditingViewportClient->ViewFOV);
				}
			}
			FEWResult Res = Capture(V, Source, MaxWidth, bInline, File, UIWidget);
			if (Res.bOk && Camera.IsValid()) { Res.Data->SetObjectField(TEXT("camera"), Camera); }
			return Res;
		});

	R.RegisterAsync(TEXT("screenshot_ab"),
		TEXT("Before/after picture in one call: capture a view (A), run steps (tool calls such as object_set or console_command) or one console command, wait, capture the same view again (B), and return both side by side with the share of pixels that changed."),
		TEXT(R"({"type":"object","properties":{
			"source":{"type":"string","enum":["game","editor"]},
			"player":{"type":"integer"},
			"steps":{"type":"array","items":{"type":"object"},"description":"[{tool, args}] run between the captures (tools that finish at once)."},
			"command":{"type":"string","description":"A console command to run between the captures."},
			"wait":{"type":"number","description":"Seconds between the change and capture B, default 0.5."},
			"maxWidth":{"type":"integer","description":"Width of each half, default 640."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args, FEWDone Done)
		{
			FString Source = EW::Str(Args, TEXT("source")).ToLower();
			if (Source.IsEmpty()) { Source = EW::IsPlaying() ? TEXT("game") : TEXT("editor"); }
			const int32 Player = (int32)EW::Num(Args, TEXT("player"), 0);
			const int32 MaxWidth = (int32)EW::Num(Args, TEXT("maxWidth"), 640);
			auto FindViewport = [Source, Player](FString& Err) -> FViewport*
			{
				if (Source == TEXT("game"))
				{
					APlayerController* PC = EW::PiePlayer(Player, Err);
					return PC && PC->GetLocalPlayer() && PC->GetLocalPlayer()->ViewportClient ? PC->GetLocalPlayer()->ViewportClient->Viewport : nullptr;
				}
				return GCurrentLevelEditingViewportClient ? GCurrentLevelEditingViewportClient->Viewport : nullptr;
			};
			const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
			Args->TryGetArrayField(TEXT("steps"), Steps);
			const FString Command = EW::Str(Args, TEXT("command"));
			if ((!Steps || Steps->Num() == 0) && Command.IsEmpty()) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("Pass steps or command (what changes between A and B)."))); return; }

			FString Err;
			FViewport* V = FindViewport(Err);
			if (!V) { Done(FEWResult::Error(Source == TEXT("game") ? TEXT("NO_PLAYER") : TEXT("NO_VIEWPORT"), Err.IsEmpty() ? TEXT("No viewport to capture.") : Err)); return; }
			TSharedRef<TArray<FColor>> A = MakeShared<TArray<FColor>>();
			int32 AW = 0, AH = 0;
			FEWResult GrabErr;
			if (!Grab(V, MaxWidth, *A, AW, AH, GrabErr)) { Done(GrabErr); return; }

			// Apply the change.
			TArray<TSharedPtr<FJsonValue>> StepResults;
			if (Steps)
			{
				for (int32 i = 0; i < Steps->Num(); ++i)
				{
					const TSharedPtr<FJsonObject>* S = nullptr;
					if (!(*Steps)[i]->TryGetObject(S)) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("steps[%d] is not an object."), i))); return; }
					FString Tool;
					(*S)->TryGetStringField(TEXT("tool"), Tool);
					const TSharedPtr<FJsonObject>* StepArgs = nullptr;
					const FEWResult One = FEWRegistry::Get().Call(Tool, (*S)->TryGetObjectField(TEXT("args"), StepArgs) ? *StepArgs : nullptr);
					TSharedRef<FJsonObject> J = One.ToJson();
					J->RemoveField(TEXT("editor"));
					J->SetStringField(TEXT("tool"), Tool);
					StepResults.Add(MakeShared<FJsonValueObject>(J));
					if (!One.bOk)
					{
						FEWResult Res = FEWResult::Error(TEXT("STEP_FAILED"), FString::Printf(TEXT("steps[%d] (%s) failed: %s No B picture taken."), i, *Tool, *One.Message));
						Res.Data = MakeShared<FJsonObject>();
						Res.Data->SetArrayField(TEXT("steps"), StepResults);
						Done(Res);
						return;
					}
				}
			}
			if (!Command.IsEmpty())
			{
				FString PErr;
				if (APlayerController* PC = Source == TEXT("game") ? EW::PiePlayer(Player, PErr) : nullptr) { PC->ConsoleCommand(Command, true); }
				else { GEditor->Exec(GEditor->GetEditorWorldContext().World(), *Command); }
			}
			const float Wait = FMath::Clamp((float)EW::Num(Args, TEXT("wait"), 0.5), 0.f, 30.f);
			EW::PollUntil(Wait, []() { return false; }, [Done, A, AW, AH, FindViewport, MaxWidth, StepResults, Source]()
			{
				FString Err;
				FViewport* V2 = FindViewport(Err);
				TArray<FColor> B;
				int32 BW = 0, BH = 0;
				FEWResult GrabErr;
				if (!V2 || !Grab(V2, MaxWidth, B, BW, BH, GrabErr)) { Done(V2 ? GrabErr : FEWResult::Error(TEXT("NO_VIEWPORT"), TEXT("The view is gone (Play stopped?)."))); return; }
				// Side by side, 8 px gap.
				const int32 Gap = 8, W = AW + Gap + BW, H = FMath::Max(AH, BH);
				TArray<FColor> Out;
				Out.Init(FColor(24, 24, 24, 255), W * H);
				for (int32 y = 0; y < AH; ++y) { FMemory::Memcpy(&Out[y * W], &(*A)[y * AW], AW * sizeof(FColor)); }
				for (int32 y = 0; y < BH; ++y) { FMemory::Memcpy(&Out[y * W + AW + Gap], &B[y * BW], BW * sizeof(FColor)); }
				int32 Changed = 0;
				if (AW == BW && AH == BH)
				{
					for (int32 i = 0; i < B.Num(); ++i)
					{
						const FColor& P = (*A)[i]; const FColor& Q = B[i];
						if (FMath::Abs(P.R - Q.R) + FMath::Abs(P.G - Q.G) + FMath::Abs(P.B - Q.B) > 30) { ++Changed; }
					}
				}
				TArray64<uint8> Png;
				EW::EncodePng(Out, W, H, Png);
				const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Enginewright/Screenshots") /
					FString::Printf(TEXT("ab_%s_%s.png"), *Source, *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S_%s"))));
				FFileHelper::SaveArrayToFile(Png, *File);
				TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("file"), File);
				Data->SetNumberField(TEXT("width"), W);
				Data->SetNumberField(TEXT("height"), H);
				Data->SetNumberField(TEXT("changedShare"), FMath::RoundToDouble(1000.0 * Changed / FMath::Max(1, B.Num())) / 1000);
				Data->SetNumberField(TEXT("brightnessA"), FMath::RoundToDouble(MeanLuma(*A) * 1000) / 1000);
				Data->SetNumberField(TEXT("brightnessB"), FMath::RoundToDouble(MeanLuma(B) * 1000) / 1000);
				Data->SetArrayField(TEXT("steps"), StepResults);
				FEWResult Res = FEWResult::Ok(Data, TEXT("Left: before (A). Right: after (B)."));
				if (Changed == 0) { Res.Warnings.Add(TEXT("No visible change between A and B.")); }
				Res.Images.Emplace(TEXT("image/png"), FBase64::Encode(Png.GetData(), Png.Num()));
				Done(Res);
			});
		});
}
