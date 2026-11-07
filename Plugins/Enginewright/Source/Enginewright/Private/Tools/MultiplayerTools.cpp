// Multiplayer test helpers: compare what each Play In Editor world (server and clients) sees, and emulate bad networks.
#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EWProps.h"

#include "Engine/Engine.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

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

	FString RoleName(ENetRole Role)
	{
		switch (Role)
		{
		case ROLE_None: return TEXT("none");
		case ROLE_SimulatedProxy: return TEXT("simulatedProxy");
		case ROLE_AutonomousProxy: return TEXT("autonomousProxy");
		case ROLE_Authority: return TEXT("authority");
		default: return TEXT("unknown");
		}
	}

	TArray<TSharedPtr<FJsonValue>> Vec(const FVector& V)
	{
		return { MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.X * 10) / 10), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Y * 10) / 10), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Z * 10) / 10) };
	}

	struct FActorQuery
	{
		UClass* Class = nullptr;
		FName Tag;
		TArray<FString> Properties;
		int32 Limit = 50;
		bool bReplicatedOnly = false;
	};

	/** One entry per PIE world: net mode, count and the matching actors. Sets OutMinCount to the smallest count. */
	TSharedRef<FJsonObject> QueryWorlds(const FActorQuery& Q, int32& OutMinCount, FString& OutPropError)
	{
		TArray<TSharedPtr<FJsonValue>> Worlds;
		OutMinCount = MAX_int32;
		TSet<int32> Counts;
		int32 Index = 0;
		for (UWorld* W : EW::PieWorlds())
		{
			TSharedRef<FJsonObject> WJ = MakeShared<FJsonObject>();
			WJ->SetNumberField(TEXT("world"), Index++);
			WJ->SetStringField(TEXT("netMode"), NetModeName(W->GetNetMode()));
			TArray<TSharedPtr<FJsonValue>> Actors;
			int32 Count = 0;
			for (TActorIterator<AActor> It(W, Q.Class); It; ++It)
			{
				AActor* A = *It;
				if (!IsValid(A) || A->IsActorBeingDestroyed()) { continue; }
				if (!Q.Tag.IsNone() && !A->ActorHasTag(Q.Tag)) { continue; }
				if (Q.bReplicatedOnly && !A->GetIsReplicated()) { continue; }
				++Count;
				if (Actors.Num() >= Q.Limit) { continue; }
				TSharedRef<FJsonObject> AJ = MakeShared<FJsonObject>();
				AJ->SetStringField(TEXT("name"), A->GetName());
				AJ->SetStringField(TEXT("label"), A->GetActorLabel());
				AJ->SetStringField(TEXT("class"), A->GetClass()->GetName());
				AJ->SetArrayField(TEXT("location"), Vec(A->GetActorLocation()));
				AJ->SetStringField(TEXT("localRole"), RoleName(A->GetLocalRole()));
				AJ->SetStringField(TEXT("remoteRole"), RoleName(A->GetRemoteRole()));
				AJ->SetBoolField(TEXT("replicates"), A->GetIsReplicated());
				AJ->SetBoolField(TEXT("hidden"), A->IsHidden());
				if (A->GetOwner()) { AJ->SetStringField(TEXT("owner"), A->GetOwner()->GetName()); }
				if (Q.Properties.Num())
				{
					TSharedPtr<FJsonObject> Values;
					const FEWResult R = EW::ReadValues(A->GetClass(), A, Q.Properties, Values);
					if (!R.bOk) { OutPropError = R.Message; }
					else if (Values.IsValid()) { AJ->SetObjectField(TEXT("values"), Values); }
				}
				Actors.Add(MakeShared<FJsonValueObject>(AJ));
			}
			WJ->SetNumberField(TEXT("count"), Count);
			WJ->SetArrayField(TEXT("actors"), Actors);
			Worlds.Add(MakeShared<FJsonValueObject>(WJ));
			OutMinCount = FMath::Min(OutMinCount, Count);
			Counts.Add(Count);
		}
		if (Worlds.Num() == 0) { OutMinCount = 0; }
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("worlds"), Worlds);
		Data->SetBoolField(TEXT("sameCountEverywhere"), Counts.Num() <= 1);
		return Data;
	}

	TSharedRef<FJsonObject> SettingsJson(const FPacketSimulationSettings& S)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetNumberField(TEXT("lagMin"), S.PktLagMin);
		J->SetNumberField(TEXT("lagMax"), S.PktLagMax);
		J->SetNumberField(TEXT("loss"), S.PktLoss);
		J->SetNumberField(TEXT("incomingLagMin"), S.PktIncomingLagMin);
		J->SetNumberField(TEXT("incomingLagMax"), S.PktIncomingLagMax);
		J->SetNumberField(TEXT("incomingLoss"), S.PktIncomingLoss);
		J->SetNumberField(TEXT("duplicate"), S.PktDup);
		J->SetBoolField(TEXT("reorder"), S.PktOrder != 0);
		return J;
	}
}

void RegisterMultiplayerTools(FEWRegistry& R)
{
	R.RegisterAsync(TEXT("pie_actors"),
		TEXT("Multiplayer check: list the actors of a class in every Play In Editor world (server and each client) with net roles, owner and chosen property values, so you can assert 'client 2 sees X'. With minCount and timeout, waits until every world has at least minCount of them (replication delay)."),
		TEXT(R"({"type":"object","properties":{
			"class":{"type":"string","description":"Short class name, /Script path or Blueprint path; subclasses match too."},
			"tag":{"type":"string"},
			"replicatedOnly":{"type":"boolean","description":"Skip actors that do not replicate (for example a client's local spectator pawn). Default false."},
			"properties":{"type":"array","items":{"type":"string"},"description":"Property names to read on each actor, e.g. [\"Health\"]."},
			"minCount":{"type":"integer","description":"Wait until every world has at least this many."},
			"timeout":{"type":"number","description":"Seconds to wait for minCount; default 10."},
			"limit":{"type":"integer","description":"Actors listed per world, default 50 (count is always complete)."}},
			"required":["class"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args, FEWDone Done)
		{
			if (!EW::IsPlaying() || EW::PieWorlds().Num() == 0) { Done(FEWResult::Error(TEXT("PIE_NOT_RUNNING"), TEXT("Play In Editor is not running (pie_start first)."))); return; }
			FActorQuery Q;
			FString Err;
			TArray<FString> Sug;
			Q.Class = EW::FindClass(EW::Str(Args, TEXT("class")), Err, &Sug);
			if (!Q.Class) { Done(FEWResult::Error(TEXT("UNKNOWN_CLASS"), Err, Sug)); return; }
			if (!Q.Class->IsChildOf(AActor::StaticClass())) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("%s is not an actor class."), *Q.Class->GetName()))); return; }
			Q.Tag = FName(*EW::Str(Args, TEXT("tag")));
			Q.Properties = EW::StrList(Args, TEXT("properties"));
			Q.bReplicatedOnly = EW::Bool(Args, TEXT("replicatedOnly"), false);
			Q.Limit = FMath::Clamp((int32)EW::Num(Args, TEXT("limit"), 50), 1, 500);
			for (const FString& P : Q.Properties)
			{
				if (!EW::FindPropertyLoose(Q.Class, P))
				{
					Done(FEWResult::Error(TEXT("UNKNOWN_PROPERTY"), FString::Printf(TEXT("%s has no property '%s'."), *Q.Class->GetName(), *P), EW::Suggest(P, EW::PropertyNames(Q.Class))));
					return;
				}
			}
			const int32 MinCount = (int32)EW::Num(Args, TEXT("minCount"), -1);
			const float Timeout = FMath::Clamp((float)EW::Num(Args, TEXT("timeout"), 10), 0.f, 300.f);
			if (Args->HasField(TEXT("minCount")) && MinCount < 0) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("minCount must be 0 or more."))); return; }

			auto Finish = [Q, MinCount](bool bTimedOut) -> FEWResult
			{
				int32 Min = 0;
				FString PropErr;
				TSharedRef<FJsonObject> Data = QueryWorlds(Q, Min, PropErr);
				if (!PropErr.IsEmpty()) { return FEWResult::Error(TEXT("READ_FAILED"), PropErr); }
				if (bTimedOut)
				{
					FEWResult Res = FEWResult::Error(TEXT("TIMEOUT"), FString::Printf(TEXT("Not every world has %d %s actor(s); the smallest count is %d. See data."), MinCount, *Q.Class->GetName(), Min));
					Res.Data = Data;
					return Res;
				}
				FEWResult Res = FEWResult::Ok(Data);
				if (!Data->GetBoolField(TEXT("sameCountEverywhere"))) { Res.Warnings.Add(TEXT("The worlds do not see the same number of these actors (not replicated, not relevant, or still arriving).")); }
				return Res;
			};

			if (MinCount < 0)
			{
				Done(Finish(false));
				return;
			}
			EW::PollUntil(Timeout,
				[Q, MinCount, Done, Finish]() -> bool
				{
					if (!EW::IsPlaying()) { Done(FEWResult::Error(TEXT("PIE_NOT_RUNNING"), TEXT("Play In Editor stopped while waiting."))); return true; }
					int32 Min = 0;
					FString PropErr;
					QueryWorlds(Q, Min, PropErr);
					if (Min < MinCount) { return false; }
					Done(Finish(false));
					return true;
				},
				[Done, Finish]() { Done(Finish(true)); });
		});

	R.Register(TEXT("pie_net_emulation"),
		TEXT("Emulate a bad network in a multiplayer Play In Editor session: profile off / average / bad, or exact values (lag in ms, loss in percent). Applied to the server and every client net driver, then read back."),
		TEXT(R"({"type":"object","properties":{
			"profile":{"type":"string","enum":["off","average","bad"],"description":"average = 30-60 ms, 1% loss; bad = 100-200 ms, 5% loss (both directions)."},
			"lagMin":{"type":"integer"},"lagMax":{"type":"integer"},"loss":{"type":"integer"},
			"incomingLagMin":{"type":"integer"},"incomingLagMax":{"type":"integer"},"incomingLoss":{"type":"integer"},
			"duplicate":{"type":"integer","description":"Percent of packets sent twice."},
			"reorder":{"type":"boolean"}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
#if DO_ENABLE_NET_TEST
			if (!EW::IsPlaying()) { return FEWResult::Error(TEXT("PIE_NOT_RUNNING"), TEXT("Play In Editor is not running (pie_start with players > 1 first).")); }
			FPacketSimulationSettings S;
			const FString Profile = EW::Str(Args, TEXT("profile")).ToLower();
			if (Profile == TEXT("average")) { S.PktLagMin = 30; S.PktLagMax = 60; S.PktLoss = 1; S.PktIncomingLagMin = 30; S.PktIncomingLagMax = 60; S.PktIncomingLoss = 1; }
			else if (Profile == TEXT("bad")) { S.PktLagMin = 100; S.PktLagMax = 200; S.PktLoss = 5; S.PktIncomingLagMin = 100; S.PktIncomingLagMax = 200; S.PktIncomingLoss = 5; }
			else if (!Profile.IsEmpty() && Profile != TEXT("off")) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("profile must be off, average or bad.")); }

			struct FField { const TCHAR* Key; int32* Target; int32 Max; };
			const FField Fields[] = {
				{ TEXT("lagMin"), &S.PktLagMin, 5000 }, { TEXT("lagMax"), &S.PktLagMax, 5000 }, { TEXT("loss"), &S.PktLoss, 100 },
				{ TEXT("incomingLagMin"), &S.PktIncomingLagMin, 5000 }, { TEXT("incomingLagMax"), &S.PktIncomingLagMax, 5000 },
				{ TEXT("incomingLoss"), &S.PktIncomingLoss, 100 }, { TEXT("duplicate"), &S.PktDup, 100 } };
			for (const FField& F : Fields)
			{
				double V = 0;
				if (!Args->TryGetNumberField(F.Key, V)) { continue; }
				if (V < 0 || V > F.Max || V != FMath::FloorToDouble(V)) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("%s must be a whole number from 0 to %d."), F.Key, F.Max)); }
				*F.Target = (int32)V;
			}
			bool bReorder = false;
			if (Args->TryGetBoolField(TEXT("reorder"), bReorder)) { S.PktOrder = bReorder ? 1 : 0; }
			if (S.PktLagMax < S.PktLagMin) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("lagMax must be at least lagMin.")); }
			if (S.PktIncomingLagMax < S.PktIncomingLagMin) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("incomingLagMax must be at least incomingLagMin.")); }

			TArray<TSharedPtr<FJsonValue>> Applied;
			int32 Index = 0;
			bool bMismatch = false;
			for (UWorld* W : EW::PieWorlds())
			{
				const int32 WorldIndex = Index++;
				UNetDriver* Driver = W->GetNetDriver();
				if (!Driver) { continue; }
				Driver->SetPacketSimulationSettings(S);
				const FPacketSimulationSettings& Back = Driver->PacketSimulationSettings;
				if (Back.PktLagMin != S.PktLagMin || Back.PktLagMax != S.PktLagMax || Back.PktLoss != S.PktLoss || Back.PktIncomingLoss != S.PktIncomingLoss) { bMismatch = true; }
				TSharedRef<FJsonObject> J = SettingsJson(Back);
				J->SetNumberField(TEXT("world"), WorldIndex);
				J->SetStringField(TEXT("netMode"), NetModeName(W->GetNetMode()));
				Applied.Add(MakeShared<FJsonValueObject>(J));
			}
			if (Applied.Num() == 0) { return FEWResult::Error(TEXT("NOT_NETWORKED"), TEXT("No PIE world has a net driver: start PIE with netMode listen or client.")); }
			if (bMismatch) { return FEWResult::Error(TEXT("READBACK_MISMATCH"), TEXT("A net driver did not keep the settings.")); }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("drivers"), Applied);
			return FEWResult::Ok(Data);
#else
			return FEWResult::Error(TEXT("NOT_SUPPORTED"), TEXT("Network emulation is compiled out of this build."));
#endif
		});
}
