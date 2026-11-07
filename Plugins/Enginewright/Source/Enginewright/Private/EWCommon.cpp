#include "EWCommon.h"

#include "Containers/Ticker.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

namespace EW
{
	bool IsPlaying()
	{
		return GEditor && (GEditor->PlayWorld != nullptr || GEditor->IsPlaySessionInProgress());
	}

	TSharedRef<FJsonObject> EditorState()
	{
		TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetBoolField(TEXT("playing"), IsPlaying());
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		S->SetStringField(TEXT("map"), World ? World->GetOutermost()->GetName() : FString());
		if (FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")) &&
			FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().IsLoadingAssets())
		{
			S->SetBoolField(TEXT("assetsLoading"), true); // Asset searches may be incomplete until the start-up scan ends.
		}
		return S;
	}

	TSharedPtr<FJsonObject> ParseObject(const FString& Text)
	{
		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Obj))
		{
			return nullptr;
		}
		return Obj;
	}

	FString ToJsonString(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	static FString NormalizeObjectPath(const FString& In)
	{
		// /Game/A/B -> /Game/A/B.B ; leaves /Game/A/B.B and sub-object paths alone.
		FString Path = In.TrimStartAndEnd();
		FString Left, Right;
		if (Path.Split(TEXT(":"), &Left, &Right))
		{
			return NormalizeObjectPath(Left) + TEXT(":") + Right;
		}
		if (!Path.Contains(TEXT(".")))
		{
			return Path + TEXT(".") + FPackageName::GetShortName(Path);
		}
		return Path;
	}

	UObject* ResolveObject(const FString& Path, bool bClassDefault, FString& OutResolvedPath, FString& OutError)
	{
		if (Path.IsEmpty() || !Path.StartsWith(TEXT("/")))
		{
			OutError = FString::Printf(TEXT("Path '%s' must start with / (for example /Game/Blueprints/BP_Door)."), *Path);
			return nullptr;
		}

		const FString ObjectPath = NormalizeObjectPath(Path);
		FString PackageName = ObjectPath;
		PackageName.Split(TEXT("."), &PackageName, nullptr);

		UObject* Obj = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath);
		if (!Obj)
		{
			if (!FindPackage(nullptr, *PackageName))
			{
				if (!FPackageName::DoesPackageExist(PackageName))
				{
					OutError = FString::Printf(TEXT("No asset at '%s'."), *PackageName);
					return nullptr;
				}
				LoadPackage(nullptr, *PackageName, LOAD_None);
			}
			Obj = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath);
		}
		if (!Obj && !ObjectPath.Contains(TEXT(":")))
		{
			Obj = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
		}
		if (!Obj)
		{
			OutError = FString::Printf(TEXT("Object '%s' not found (package '%s' exists)."), *ObjectPath, *PackageName);
			return nullptr;
		}

		if (bClassDefault)
		{
			if (UBlueprint* BP = Cast<UBlueprint>(Obj))
			{
				Obj = BP->GeneratedClass ? BP->GeneratedClass->GetDefaultObject() : nullptr;
			}
			else if (UClass* Cls = Cast<UClass>(Obj))
			{
				Obj = Cls->GetDefaultObject();
			}
			if (!Obj)
			{
				OutError = FString::Printf(TEXT("'%s' has no generated class yet; compile the Blueprint first."), *ObjectPath);
				return nullptr;
			}
		}

		OutResolvedPath = Obj->GetPathName();
		return Obj;
	}

	UClass* FindClass(const FString& InName, FString& OutError, TArray<FString>* OutSuggest)
	{
		const FString Name = InName.TrimStartAndEnd();
		if (Name.IsEmpty()) { OutError = TEXT("Class name is empty."); return nullptr; }
		if (Name.StartsWith(TEXT("/Script/")))
		{
			if (UClass* C = FindObject<UClass>(nullptr, *Name)) { return C; }
			OutError = FString::Printf(TEXT("No class '%s'."), *Name);
			return nullptr;
		}
		if (Name.StartsWith(TEXT("/")))
		{
			FString Resolved;
			UObject* Obj = ResolveObject(Name, false, Resolved, OutError);
			if (UBlueprint* BP = Cast<UBlueprint>(Obj))
			{
				if (BP->GeneratedClass) { return BP->GeneratedClass; }
				OutError = FString::Printf(TEXT("Blueprint '%s' has no generated class; compile it first."), *Name);
				return nullptr;
			}
			if (UClass* C = Cast<UClass>(Obj)) { return C; }
			if (Obj) { OutError = FString::Printf(TEXT("'%s' is a %s, not a Blueprint or class."), *Name, *Obj->GetClass()->GetName()); }
			return nullptr;
		}
		FString Short = Name;
		if (UClass* C = FindFirstObject<UClass>(*Short, EFindFirstObjectOptions::None)) { return C; }
		// Accept the C++ prefix (AActor, UStaticMeshComponent) too.
		if (Short.Len() > 1 && (Short[0] == TEXT('A') || Short[0] == TEXT('U')) && FChar::IsUpper(Short[1]))
		{
			if (UClass* C = FindFirstObject<UClass>(*Short.RightChop(1), EFindFirstObjectOptions::None)) { return C; }
		}
		if (UClass* C = FindFirstObject<UClass>(*(Short + TEXT("_C")), EFindFirstObjectOptions::None)) { return C; }
		OutError = FString::Printf(TEXT("No class '%s'. Use a short name (StaticMeshActor), /Script/Module.Class or a Blueprint path."), *Name);
		if (OutSuggest)
		{
			TArray<FString> Names;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (!It->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists)) { Names.Add(It->GetName()); }
			}
			*OutSuggest = Suggest(Short, Names);
		}
		return nullptr;
	}

	static int32 EditDistance(const FString& A, const FString& B)
	{
		const FString X = A.ToLower();
		const FString Y = B.ToLower();
		TArray<int32> Prev, Cur;
		Prev.SetNum(Y.Len() + 1);
		Cur.SetNum(Y.Len() + 1);
		for (int32 j = 0; j <= Y.Len(); ++j) { Prev[j] = j; }
		for (int32 i = 1; i <= X.Len(); ++i)
		{
			Cur[0] = i;
			for (int32 j = 1; j <= Y.Len(); ++j)
			{
				const int32 Cost = X[i - 1] == Y[j - 1] ? 0 : 1;
				Cur[j] = FMath::Min3(Prev[j] + 1, Cur[j - 1] + 1, Prev[j - 1] + Cost);
			}
			Swap(Prev, Cur);
		}
		return Prev[Y.Len()];
	}

	TArray<FString> Suggest(const FString& Wanted, const TArray<FString>& Candidates, int32 Max)
	{
		TArray<TPair<int32, FString>> Scored;
		for (const FString& C : Candidates)
		{
			if (FMath::Abs(C.Len() - Wanted.Len()) > FMath::Max(8, Wanted.Len())) { continue; }
			int32 D = EditDistance(Wanted, C);
			if (C.Contains(Wanted, ESearchCase::IgnoreCase) || Wanted.Contains(C, ESearchCase::IgnoreCase))
			{
				D = FMath::Min(D, 1);
			}
			Scored.Emplace(D, C);
		}
		Scored.StableSort([](const TPair<int32, FString>& L, const TPair<int32, FString>& R) { return L.Key < R.Key; });
		TArray<FString> Out;
		for (const TPair<int32, FString>& S : Scored)
		{
			if (Out.Num() >= Max || S.Key > FMath::Max(3, Wanted.Len() / 2)) { break; }
			Out.AddUnique(S.Value);
		}
		return Out;
	}

	FString Str(const TSharedPtr<FJsonObject>& Args, const FString& Key)
	{
		FString V;
		if (Args.IsValid())
		{
			Args->TryGetStringField(Key, V);
		}
		return V;
	}

	TArray<FString> StrList(const TSharedPtr<FJsonObject>& Args, const FString& Key)
	{
		TArray<FString> Out;
		if (!Args.IsValid()) { return Out; }
		const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
		if (Args->TryGetArrayField(Key, List))
		{
			for (const TSharedPtr<FJsonValue>& V : *List) { Out.Add(V->AsString()); }
		}
		else
		{
			FString One;
			if (Args->TryGetStringField(Key, One)) { Out.Add(One); }
		}
		return Out;
	}

	double Num(const TSharedPtr<FJsonObject>& Args, const FString& Key, double Default)
	{
		double V = Default;
		if (Args.IsValid()) { Args->TryGetNumberField(Key, V); }
		return V;
	}

	bool Bool(const TSharedPtr<FJsonObject>& Args, const FString& Key, bool Default)
	{
		bool V = Default;
		if (Args.IsValid()) { Args->TryGetBoolField(Key, V); }
		return V;
	}

	bool SplitAssetPath(const FString& InPath, FString& OutFolder, FString& OutName, FString& OutError)
	{
		FString Path = InPath.TrimStartAndEnd();
		int32 Dot;
		if (Path.FindChar(TEXT('.'), Dot)) { Path = Path.Left(Dot); }
		if (!Path.StartsWith(TEXT("/")) || Path.EndsWith(TEXT("/")) || !FPackageName::IsValidLongPackageName(Path, false))
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid asset path; use /Game/Folder/AssetName."), *InPath);
			return false;
		}
		OutFolder = FPackageName::GetLongPackagePath(Path);
		OutName = FPackageName::GetShortName(Path);
		return true;
	}

	bool AssetPathTaken(const FString& InPath)
	{
		FString Path = InPath;
		int32 Dot;
		if (Path.FindChar(TEXT('.'), Dot)) { Path = Path.Left(Dot); }
		if (FPackageName::DoesPackageExist(Path)) { return true; }
		UPackage* Pkg = FindPackage(nullptr, *Path);
		if (!Pkg) { return false; }
		UObject* Asset = StaticFindObject(UObject::StaticClass(), Pkg, *FPackageName::GetShortName(Path));
		return IsValid(Asset);
	}

	TArray<UWorld*> PieWorlds()
	{
		TArray<UWorld*> Out;
		if (!GEditor) { return Out; }
		TArray<TPair<int32, UWorld*>> Sorted;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::PIE && Ctx.World()) { Sorted.Emplace(Ctx.PIEInstance, Ctx.World()); }
		}
		Sorted.Sort([](const auto& A, const auto& B) { return A.Key < B.Key; });
		for (const auto& P : Sorted) { Out.Add(P.Value); }
		return Out;
	}

	APlayerController* PiePlayer(int32 Index, FString& OutError)
	{
		if (!IsPlaying()) { OutError = TEXT("Play In Editor is not running (pie_start first)."); return nullptr; }
		TArray<APlayerController*> Players;
		for (UWorld* W : PieWorlds())
		{
			for (FConstPlayerControllerIterator It = W->GetPlayerControllerIterator(); It; ++It)
			{
				if (APlayerController* PC = It->Get())
				{
					if (PC->IsLocalController()) { Players.Add(PC); }
				}
			}
		}
		if (!Players.IsValidIndex(Index))
		{
			OutError = FString::Printf(TEXT("No local player %d; %d player(s) exist."), Index, Players.Num());
			return nullptr;
		}
		return Players[Index];
	}

	bool EncodePng(const TArray<FColor>& Pixels, int32 Width, int32 Height, TArray64<uint8>& OutPng)
	{
		IImageWrapperModule& M = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> W = M.CreateImageWrapper(EImageFormat::PNG);
		if (!W.IsValid() || !W->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8)) { return false; }
		OutPng = W->GetCompressed();
		return OutPng.Num() > 0;
	}

	void PollUntil(float TimeoutSeconds, TFunction<bool()> Fn, TFunction<void()> OnTimeout)
	{
		const double End = FPlatformTime::Seconds() + TimeoutSeconds;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Fn, OnTimeout, End](float) -> bool
		{
			if (Fn()) { return false; }
			if (FPlatformTime::Seconds() > End) { OnTimeout(); return false; }
			return true;
		}));
	}
}
