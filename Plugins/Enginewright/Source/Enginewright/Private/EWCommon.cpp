#include "EWCommon.h"

#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

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
			int32 D = EditDistance(Wanted, C);
			if (C.Contains(Wanted, ESearchCase::IgnoreCase) || Wanted.Contains(C, ESearchCase::IgnoreCase))
			{
				D = FMath::Min(D, 1);
			}
			Scored.Emplace(D, C);
		}
		Scored.Sort([](const TPair<int32, FString>& L, const TPair<int32, FString>& R) { return L.Key < R.Key; });
		TArray<FString> Out;
		for (const TPair<int32, FString>& S : Scored)
		{
			if (Out.Num() >= Max || S.Key > FMath::Max(3, Wanted.Len() / 2)) { break; }
			Out.Add(S.Value);
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
}
