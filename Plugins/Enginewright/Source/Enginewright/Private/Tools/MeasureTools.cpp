// Measuring helpers: the size of a class without placing it, and how much of the screen an actor covers in play.
#include "EnginewrightTool.h"
#include "EWCommon.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/PrimitiveComponent.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "UObject/Package.h"

namespace
{
	TArray<TSharedPtr<FJsonValue>> Vec(const FVector& V)
	{
		return { MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.X * 10) / 10), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Y * 10) / 10), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Z * 10) / 10) };
	}

	double Round2(double V) { return FMath::RoundToDouble(V * 100) / 100; }

	AActor* FindInWorld(UWorld* World, const FString& Key, UClass* Class)
	{
		for (TActorIterator<AActor> It(World, Class ? Class : AActor::StaticClass()); It; ++It)
		{
			if (!IsValid(*It)) { continue; }
			if (Key.IsEmpty() || It->GetActorLabel() == Key || It->GetName() == Key || It->GetActorLabel().MatchesWildcard(Key)) { return *It; }
		}
		return nullptr;
	}
}

void RegisterMeasureTools(FEWRegistry& R)
{
	R.Register(TEXT("class_measure"),
		TEXT("Measure an actor class or Blueprint without placing it: spawns a temporary hidden copy in the editor world, reads its bounds (all components and colliding only) and each component's size, then removes it. The level is not changed."),
		TEXT(R"({"type":"object","properties":{
			"class":{"type":"string","description":"Blueprint path (/Game/Items/BP_Apple), /Script path or short class name."}},
			"required":["class"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Err;
			TArray<FString> Sug;
			UClass* Class = EW::FindClass(EW::Str(Args, TEXT("class")), Err, &Sug);
			if (!Class) { return FEWResult::Error(TEXT("UNKNOWN_CLASS"), Err, Sug); }
			if (!Class->IsChildOf(AActor::StaticClass())) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("%s is not an actor class."), *Class->GetName())); }
			if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("%s is abstract or outdated and cannot be spawned."), *Class->GetName())); }
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World) { return FEWResult::Error(TEXT("NO_LEVEL"), TEXT("No editor level is open.")); }

			UPackage* LevelPackage = World->PersistentLevel ? World->PersistentLevel->GetOutermost() : nullptr;
			const bool bWasDirty = LevelPackage && LevelPackage->IsDirty();
			FActorSpawnParameters P;
			P.ObjectFlags = RF_Transient;
			P.bTemporaryEditorActor = true;
			P.bHideFromSceneOutliner = true;
			P.bNoFail = true;
			P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AActor* A = World->SpawnActor<AActor>(Class, FTransform::Identity, P);
			if (!A) { return FEWResult::Error(TEXT("SPAWN_FAILED"), FString::Printf(TEXT("The engine could not spawn %s."), *Class->GetName())); }

			// Bounds of what the game shows or collides with: editor-only parts (sprites, arrows) are left out.
			FBox All(ForceInit), Colliding(ForceInit);
			TArray<TSharedPtr<FJsonValue>> Comps;
			TArray<UPrimitiveComponent*> Prims;
			A->GetComponents(Prims, true);
			for (UPrimitiveComponent* C : Prims)
			{
				if (!C || !C->IsRegistered() || C->IsEditorOnly()) { continue; }
				const FBoxSphereBounds B = C->Bounds;
				const bool bCollides = C->IsCollisionEnabled();
				All += B.GetBox();
				if (bCollides) { Colliding += B.GetBox(); }
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("name"), C->GetName());
				J->SetStringField(TEXT("class"), C->GetClass()->GetName());
				J->SetArrayField(TEXT("center"), Vec(B.Origin));
				J->SetArrayField(TEXT("size"), Vec(B.BoxExtent * 2.0));
				J->SetBoolField(TEXT("collides"), bCollides);
				J->SetBoolField(TEXT("visible"), C->IsVisible() && !C->bHiddenInGame);
				Comps.Add(MakeShared<FJsonValueObject>(J));
			}
			const FVector Origin = All.IsValid ? All.GetCenter() : FVector::ZeroVector, Extent = All.IsValid ? All.GetExtent() : FVector::ZeroVector;
			const FVector COrigin = Colliding.IsValid ? Colliding.GetCenter() : FVector::ZeroVector, CExtent = Colliding.IsValid ? Colliding.GetExtent() : FVector::ZeroVector;

			World->DestroyActor(A);
			if (LevelPackage && !bWasDirty) { LevelPackage->SetDirtyFlag(false); }

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("class"), Class->GetPathName());
			Data->SetArrayField(TEXT("center"), Vec(Origin));
			Data->SetArrayField(TEXT("size"), Vec(Extent * 2.0));
			Data->SetArrayField(TEXT("collisionCenter"), Vec(COrigin));
			Data->SetArrayField(TEXT("collisionSize"), Vec(CExtent * 2.0));
			Data->SetArrayField(TEXT("components"), Comps);
			FEWResult Res = FEWResult::Ok(Data);
			if (Extent.IsNearlyZero()) { Res.Warnings.Add(TEXT("Size is zero: the class has no visible or colliding component (or its mesh is not set).")); }
			return Res;
		});

	R.Register(TEXT("screen_coverage"),
		TEXT("Readability check in Play In Editor: how big an actor looks from a player's camera. Projects the actor's bounds to that player's screen and returns the box in pixels, its share of the screen, whether it is fully on screen, and the camera distance. Occlusion is not checked; use screenshot to see it."),
		TEXT(R"({"type":"object","properties":{
			"actor":{"type":"string","description":"Actor label or name in the play world (wildcards allowed); the first match is used."},
			"class":{"type":"string","description":"Or: the first actor of this class."},
			"player":{"type":"integer","description":"Local player index, default 0."},
			"collidingOnly":{"type":"boolean","description":"Measure only components that collide (default false: every visible component)."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Err;
			APlayerController* PC = EW::PiePlayer((int32)EW::Num(Args, TEXT("player"), 0), Err);
			if (!PC) { return FEWResult::Error(TEXT("NO_PLAYER"), Err); }
			const FString Key = EW::Str(Args, TEXT("actor"));
			const FString ClassArg = EW::Str(Args, TEXT("class"));
			if (Key.IsEmpty() && ClassArg.IsEmpty()) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("Pass actor or class.")); }
			UClass* Class = nullptr;
			if (!ClassArg.IsEmpty())
			{
				TArray<FString> Sug;
				Class = EW::FindClass(ClassArg, Err, &Sug);
				if (!Class) { return FEWResult::Error(TEXT("UNKNOWN_CLASS"), Err, Sug); }
			}
			AActor* A = FindInWorld(PC->GetWorld(), Key, Class);
			if (!A) { return FEWResult::Error(TEXT("ACTOR_NOT_FOUND"), FString::Printf(TEXT("No actor matching '%s' in player %d's world."), *(Key.IsEmpty() ? ClassArg : Key), (int32)EW::Num(Args, TEXT("player"), 0))); }

			int32 VW = 0, VH = 0;
			PC->GetViewportSize(VW, VH);
			if (VW <= 0 || VH <= 0) { return FEWResult::Error(TEXT("NO_VIEWPORT"), TEXT("The player has no viewport size (window minimized?).")); }
			FVector Origin, Extent;
			A->GetActorBounds(EW::Bool(Args, TEXT("collidingOnly"), false), Origin, Extent, true);
			const FBox Box(Origin - Extent, Origin + Extent);
			FVector2D Min(TNumericLimits<double>::Max()), Max(-TNumericLimits<double>::Max());
			int32 Behind = 0;
			for (int32 i = 0; i < 8; ++i)
			{
				const FVector Corner((i & 1) ? Box.Max.X : Box.Min.X, (i & 2) ? Box.Max.Y : Box.Min.Y, (i & 4) ? Box.Max.Z : Box.Min.Z);
				FVector2D S;
				if (!PC->ProjectWorldLocationToScreen(Corner, S, true)) { ++Behind; continue; }
				Min = FVector2D(FMath::Min(Min.X, S.X), FMath::Min(Min.Y, S.Y));
				Max = FVector2D(FMath::Max(Max.X, S.X), FMath::Max(Max.Y, S.Y));
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("actor"), A->GetActorLabel());
			Data->SetNumberField(TEXT("viewportWidth"), VW);
			Data->SetNumberField(TEXT("viewportHeight"), VH);
			const FVector Cam = PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;
			Data->SetNumberField(TEXT("cameraDistance"), FMath::RoundToDouble(FVector::Dist(Cam, Origin)));
			Data->SetArrayField(TEXT("worldSize"), Vec(Extent * 2.0));
			if (Behind == 8)
			{
				Data->SetBoolField(TEXT("onScreen"), false);
				Data->SetNumberField(TEXT("screenPercent"), 0);
				return FEWResult::Ok(Data, TEXT("The actor is behind the camera."));
			}
			const double CX0 = FMath::Clamp(Min.X, 0.0, (double)VW), CX1 = FMath::Clamp(Max.X, 0.0, (double)VW);
			const double CY0 = FMath::Clamp(Min.Y, 0.0, (double)VH), CY1 = FMath::Clamp(Max.Y, 0.0, (double)VH);
			const double Visible = FMath::Max(0.0, CX1 - CX0) * FMath::Max(0.0, CY1 - CY0);
			Data->SetBoolField(TEXT("onScreen"), Visible > 0);
			Data->SetBoolField(TEXT("fullyOnScreen"), Behind == 0 && Min.X >= 0 && Min.Y >= 0 && Max.X <= VW && Max.Y <= VH);
			TArray<TSharedPtr<FJsonValue>> Rect = { MakeShared<FJsonValueNumber>(FMath::RoundToDouble(Min.X)), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(Min.Y)),
				MakeShared<FJsonValueNumber>(FMath::RoundToDouble(Max.X)), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(Max.Y)) };
			Data->SetArrayField(TEXT("screenRect"), Rect);
			Data->SetNumberField(TEXT("widthPx"), FMath::RoundToDouble(Max.X - Min.X));
			Data->SetNumberField(TEXT("heightPx"), FMath::RoundToDouble(Max.Y - Min.Y));
			Data->SetNumberField(TEXT("screenPercent"), Round2(100.0 * Visible / ((double)VW * VH)));
			FEWResult Res = FEWResult::Ok(Data);
			if (Behind > 0) { Res.Warnings.Add(TEXT("Part of the actor is behind the camera; the box is approximate.")); }
			return Res;
		});
}
