#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EWProps.h"

#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

namespace
{
	UWorld* EditorWorld() { return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr; }

	UClass* ResolveClass(const FString& In, FString& OutErr)
	{
		if (In.StartsWith(TEXT("/Script/")))
		{
			if (UClass* C = FindObject<UClass>(nullptr, *In)) { return C; }
		}
		else if (In.StartsWith(TEXT("/")))
		{
			FString Resolved;
			UObject* Obj = EW::ResolveObject(In, false, Resolved, OutErr);
			if (UBlueprint* BP = Cast<UBlueprint>(Obj)) { return BP->GeneratedClass; }
			if (UClass* C = Cast<UClass>(Obj)) { return C; }
			if (Obj) { OutErr = FString::Printf(TEXT("'%s' is not a Blueprint or class."), *In); }
			return nullptr;
		}
		else if (UClass* C = FindFirstObject<UClass>(*In, EFindFirstObjectOptions::None))
		{
			return C;
		}
		OutErr = FString::Printf(TEXT("No class '%s'. Use a short name (PointLight), /Script/Module.Class or a Blueprint path."), *In);
		return nullptr;
	}

	bool ReadVector(const TSharedPtr<FJsonObject>& Args, const FString& Key, FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Args->TryGetArrayField(Key, Arr) && Arr->Num() == 3)
		{
			Out = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
			return true;
		}
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Args->TryGetObjectField(Key, Obj))
		{
			double X = 0, Y = 0, Z = 0;
			(*Obj)->TryGetNumberField(TEXT("x"), X); (*Obj)->TryGetNumberField(TEXT("y"), Y); (*Obj)->TryGetNumberField(TEXT("z"), Z);
			(*Obj)->TryGetNumberField(TEXT("pitch"), X); (*Obj)->TryGetNumberField(TEXT("yaw"), Y); (*Obj)->TryGetNumberField(TEXT("roll"), Z);
			Out = FVector(X, Y, Z);
			return true;
		}
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Vec(const FVector& V)
	{
		return { MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z) };
	}

	AActor* FindActor(UWorld* World, const FString& Key)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->GetActorLabel() == Key || It->GetName() == Key || It->GetPathName() == Key) { return *It; }
		}
		return nullptr;
	}

	TArray<FString> ActorLabels(UWorld* World)
	{
		TArray<FString> Out;
		for (TActorIterator<AActor> It(World); It; ++It) { Out.Add(It->GetActorLabel()); }
		return Out;
	}

	TSharedRef<FJsonObject> Describe(AActor* A)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("label"), A->GetActorLabel());
		J->SetStringField(TEXT("name"), A->GetName());
		J->SetStringField(TEXT("path"), A->GetPathName());
		J->SetStringField(TEXT("class"), A->GetClass()->GetPathName());
		J->SetArrayField(TEXT("location"), Vec(A->GetActorLocation()));
		const FRotator R = A->GetActorRotation();
		J->SetArrayField(TEXT("rotation"), Vec(FVector(R.Pitch, R.Yaw, R.Roll)));
		J->SetArrayField(TEXT("scale"), Vec(A->GetActorScale3D()));
		return J;
	}

	FEWResult NoActor(UWorld* World, const FString& Key)
	{
		return FEWResult::Error(TEXT("ACTOR_NOT_FOUND"), FString::Printf(TEXT("No actor '%s' in the open level (use the label shown in the Outliner)."), *Key),
			EW::Suggest(Key, ActorLabels(World)));
	}
}

void RegisterSceneTools(FEWRegistry& R)
{
	R.Register(TEXT("actor_find"),
		TEXT("Find actors in the open editor level by class, label wildcard or tag."),
		TEXT(R"({"type":"object","properties":{
			"class":{"type":"string","description":"Short class name, /Script path or Blueprint path; subclasses match too."},
			"label":{"type":"string","description":"Outliner label wildcard, e.g. *Stove*."},
			"tag":{"type":"string"},
			"limit":{"type":"integer","description":"Default 100."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			UWorld* World = EditorWorld();
			if (!World) { return FEWResult::Error(TEXT("NO_LEVEL"), TEXT("No editor level is open.")); }
			UClass* Class = AActor::StaticClass();
			const FString ClassArg = EW::Str(Args, TEXT("class"));
			if (!ClassArg.IsEmpty())
			{
				FString Err;
				Class = ResolveClass(ClassArg, Err);
				if (!Class) { return FEWResult::Error(TEXT("UNKNOWN_CLASS"), Err); }
			}
			const FString Label = EW::Str(Args, TEXT("label"));
			const FName Tag(*EW::Str(Args, TEXT("tag")));
			int32 Limit = 100;
			Args->TryGetNumberField(TEXT("limit"), Limit);
			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 Total = 0;
			for (TActorIterator<AActor> It(World, Class); It; ++It)
			{
				if (!Label.IsEmpty() && !It->GetActorLabel().MatchesWildcard(Label)) { continue; }
				if (!Tag.IsNone() && !It->ActorHasTag(Tag)) { continue; }
				++Total;
				if (Rows.Num() < Limit) { Rows.Add(MakeShared<FJsonValueObject>(Describe(*It))); }
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("actors"), Rows);
			Data->SetNumberField(TEXT("total"), Total);
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("actor_spawn"),
		TEXT("Place an actor in the open editor level (one undo step). The level is not saved; use asset_save."),
		TEXT(R"({"type":"object","properties":{
			"class":{"type":"string","description":"Short class name, /Script path or Blueprint path."},
			"label":{"type":"string"},
			"location":{"description":"[x,y,z] or {x,y,z}."},
			"rotation":{"description":"[pitch,yaw,roll] or {pitch,yaw,roll}."},
			"scale":{"description":"[x,y,z]."}},
			"required":["class"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			UWorld* World = EditorWorld();
			if (!World) { return FEWResult::Error(TEXT("NO_LEVEL"), TEXT("No editor level is open.")); }
			FString Err;
			UClass* Class = ResolveClass(EW::Str(Args, TEXT("class")), Err);
			if (!Class) { return FEWResult::Error(TEXT("UNKNOWN_CLASS"), Err); }
			if (!Class->IsChildOf(AActor::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
			{
				return FEWResult::Error(TEXT("NOT_SPAWNABLE"), FString::Printf(TEXT("%s is not a placeable actor class."), *Class->GetName()));
			}
			FVector Loc = FVector::ZeroVector, Rot = FVector::ZeroVector, Scale = FVector::OneVector;
			ReadVector(Args, TEXT("location"), Loc);
			ReadVector(Args, TEXT("rotation"), Rot);
			ReadVector(Args, TEXT("scale"), Scale);

			FScopedTransaction Tx(FText::FromString(TEXT("Enginewright actor_spawn")));
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			const FTransform Xf(FRotator(Rot.X, Rot.Y, Rot.Z), Loc, Scale);
			AActor* A = World->SpawnActor(Class, &Xf, Params);
			if (!A) { EW::CancelUnlessBatch(Tx); return FEWResult::Error(TEXT("SPAWN_FAILED"), TEXT("The engine did not spawn the actor.")); }
			const FString Label = EW::Str(Args, TEXT("label"));
			if (!Label.IsEmpty()) { A->SetActorLabel(Label); }
			A->MarkPackageDirty();
			return FEWResult::Ok(Describe(A));
		});

	R.Register(TEXT("actor_set_transform"),
		TEXT("Move, rotate or scale an actor in the open editor level (one undo step). Give only the parts to change."),
		TEXT(R"({"type":"object","properties":{
			"actor":{"type":"string","description":"Label, name or path."},
			"location":{},"rotation":{},"scale":{}},
			"required":["actor"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			UWorld* World = EditorWorld();
			const FString Key = EW::Str(Args, TEXT("actor"));
			AActor* A = World ? FindActor(World, Key) : nullptr;
			if (!A) { return NoActor(World, Key); }
			FScopedTransaction Tx(FText::FromString(TEXT("Enginewright actor_set_transform")));
			A->Modify();
			FVector V;
			if (ReadVector(Args, TEXT("location"), V)) { A->SetActorLocation(V); }
			if (ReadVector(Args, TEXT("rotation"), V)) { A->SetActorRotation(FRotator(V.X, V.Y, V.Z)); }
			if (ReadVector(Args, TEXT("scale"), V)) { A->SetActorScale3D(V); }
			A->PostEditMove(true);
			A->MarkPackageDirty();
			return FEWResult::Ok(Describe(A));
		});

	R.Register(TEXT("actor_delete"),
		TEXT("Remove actors from the open editor level (one undo step). All must exist, otherwise nothing is removed."),
		TEXT(R"({"type":"object","properties":{"actors":{"type":"array","items":{"type":"string"}}},"required":["actors"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			UWorld* World = EditorWorld();
			const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
			Args->TryGetArrayField(TEXT("actors"), List);
			TArray<AActor*> Found;
			for (const auto& V : *List)
			{
				AActor* A = World ? FindActor(World, V->AsString()) : nullptr;
				if (!A) { FEWResult E = NoActor(World, V->AsString()); E.Message += TEXT(" Nothing was removed."); return E; }
				Found.Add(A);
			}
			FScopedTransaction Tx(FText::FromString(TEXT("Enginewright actor_delete")));
			for (AActor* A : Found) { World->EditorDestroyActor(A, true); }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("removed"), Found.Num());
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("actor_bounds"),
		TEXT("Measure an actor: world bounds center and full size in cm (components included)."),
		TEXT(R"({"type":"object","properties":{"actor":{"type":"string"},"collidingOnly":{"type":"boolean"}},"required":["actor"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			UWorld* World = EditorWorld();
			const FString Key = EW::Str(Args, TEXT("actor"));
			AActor* A = World ? FindActor(World, Key) : nullptr;
			if (!A) { return NoActor(World, Key); }
			bool bColliding = false;
			Args->TryGetBoolField(TEXT("collidingOnly"), bColliding);
			FVector Origin, Extent;
			A->GetActorBounds(bColliding, Origin, Extent);
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("center"), Vec(Origin));
			Data->SetArrayField(TEXT("size"), Vec(Extent * 2.0));
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("world_trace"),
		TEXT("Cast a line through the open level (visibility channel) and report the first hit: actor, point and distance."),
		TEXT(R"({"type":"object","properties":{"start":{},"end":{}},"required":["start","end"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			UWorld* World = EditorWorld();
			FVector S, E;
			if (!World || !ReadVector(Args, TEXT("start"), S) || !ReadVector(Args, TEXT("end"), E))
			{
				return FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("start and end must be [x,y,z]."));
			}
			FHitResult Hit;
			const bool bHit = World->LineTraceSingleByChannel(Hit, S, E, ECC_Visibility);
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("hit"), bHit);
			if (bHit)
			{
				Data->SetStringField(TEXT("actor"), Hit.GetActor() ? Hit.GetActor()->GetActorLabel() : FString());
				Data->SetArrayField(TEXT("point"), Vec(Hit.ImpactPoint));
				Data->SetNumberField(TEXT("distance"), Hit.Distance);
			}
			return FEWResult::Ok(Data);
		});
}
