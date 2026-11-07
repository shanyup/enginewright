#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EWProps.h"

#include "Engine/DeveloperSettings.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/EngineVersion.h"
#include "Misc/OutputDevice.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "UObject/UObjectIterator.h"

// ---------------------------------------------------------------------------------------------
// Log capture: keeps the last lines from every thread with a sequence number, so clients can poll.
namespace
{
	struct FLine
	{
		uint64 Seq;
		FString Category;
		ELogVerbosity::Type Verbosity;
		FString Text;
	};

	class FEWLogCapture : public FOutputDevice
	{
	public:
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			FScopeLock Lock(&Guard);
			Lines.Add({ ++Seq, Category.ToString(), Verbosity, V });
			if (Lines.Num() > 5000) { Lines.RemoveAt(0, 1000, EAllowShrinking::No); }
		}
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		FCriticalSection Guard;
		TArray<FLine> Lines;
		uint64 Seq = 0;
	};

	FEWLogCapture* GCapture = nullptr;

	ELogVerbosity::Type ParseVerbosity(const FString& S)
	{
		if (S.Equals(TEXT("Error"), ESearchCase::IgnoreCase)) { return ELogVerbosity::Error; }
		if (S.Equals(TEXT("Warning"), ESearchCase::IgnoreCase)) { return ELogVerbosity::Warning; }
		if (S.Equals(TEXT("Display"), ESearchCase::IgnoreCase)) { return ELogVerbosity::Display; }
		return ELogVerbosity::VeryVerbose;
	}

	const TCHAR* VerbosityName(ELogVerbosity::Type V)
	{
		switch (V & ELogVerbosity::VerbosityMask)
		{
		case ELogVerbosity::Fatal: return TEXT("Fatal");
		case ELogVerbosity::Error: return TEXT("Error");
		case ELogVerbosity::Warning: return TEXT("Warning");
		case ELogVerbosity::Display: return TEXT("Display");
		default: return TEXT("Log");
		}
	}

	UClass* FindSettingsClass(const FString& In, TArray<FString>& OutSuggest)
	{
		UClass* C = In.StartsWith(TEXT("/Script/")) ? FindObject<UClass>(nullptr, *In) : FindFirstObject<UClass>(*In, EFindFirstObjectOptions::None);
		if (!C && In.StartsWith(TEXT("U"))) { C = FindFirstObject<UClass>(*In.RightChop(1), EFindFirstObjectOptions::None); }
		if (C && C->HasAnyClassFlags(CLASS_Config)) { return C; }
		TArray<FString> Names;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf(UDeveloperSettings::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract)) { Names.Add(It->GetName()); }
		}
		OutSuggest = EW::Suggest(In, Names, 5);
		return nullptr;
	}

	int32 CountZenSessions(FString& OutDir)
	{
		const FString Local = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		OutDir = Local / TEXT("UnrealEngine/Common/Zen/Data/sessions");
		int32 Count = 0;
		IFileManager::Get().IterateDirectory(*OutDir, [&Count](const TCHAR*, bool) { ++Count; return true; });
		return Count;
	}
}

namespace EW
{
	void InstallLogCapture()
	{
		if (!GCapture && GLog)
		{
			GCapture = new FEWLogCapture();
			GLog->AddOutputDevice(GCapture);
		}
	}

	void RemoveLogCapture()
	{
		if (GCapture && GLog)
		{
			GLog->RemoveOutputDevice(GCapture);
			delete GCapture;
			GCapture = nullptr;
		}
	}
}

void RegisterSettingsLogTools(FEWRegistry& R)
{
	R.Register(TEXT("log_read"),
		TEXT("Read editor log lines captured since the editor started, filtered by text, category and minimum severity. Pass since (the lastSeq of the previous reply) to get only new lines, e.g. to wait for a line after pie_start."),
		TEXT(R"({"type":"object","properties":{
			"pattern":{"type":"string","description":"Case-insensitive text the line must contain."},
			"category":{"type":"string","description":"Log category, e.g. LogTemp or LogBlueprint."},
			"minVerbosity":{"type":"string","enum":["Error","Warning","Display","Log"]},
			"since":{"type":"number","description":"Only lines after this sequence number."},
			"limit":{"type":"integer","description":"Newest lines returned, default 100."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			if (!GCapture) { return FEWResult::Error(TEXT("NO_LOG"), TEXT("Log capture is not running.")); }
			const FString Pattern = EW::Str(Args, TEXT("pattern"));
			const FString Category = EW::Str(Args, TEXT("category"));
			const ELogVerbosity::Type MinV = ParseVerbosity(EW::Str(Args, TEXT("minVerbosity")));
			double Since = 0;
			Args->TryGetNumberField(TEXT("since"), Since);
			int32 Limit = 100;
			Args->TryGetNumberField(TEXT("limit"), Limit);

			TArray<FLine> Matched;
			uint64 LastSeq = 0;
			{
				FScopeLock Lock(&GCapture->Guard);
				LastSeq = GCapture->Seq;
				for (const FLine& L : GCapture->Lines)
				{
					if (L.Seq <= (uint64)Since) { continue; }
					if ((L.Verbosity & ELogVerbosity::VerbosityMask) > MinV) { continue; }
					if (!Category.IsEmpty() && !L.Category.Equals(Category, ESearchCase::IgnoreCase)) { continue; }
					if (!Pattern.IsEmpty() && !L.Text.Contains(Pattern)) { continue; }
					Matched.Add(L);
				}
			}
			const int32 First = FMath::Max(0, Matched.Num() - Limit);
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (int32 i = First; i < Matched.Num(); ++i)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetNumberField(TEXT("seq"), (double)Matched[i].Seq);
				Row->SetStringField(TEXT("category"), Matched[i].Category);
				Row->SetStringField(TEXT("verbosity"), VerbosityName(Matched[i].Verbosity));
				Row->SetStringField(TEXT("text"), Matched[i].Text);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("lines"), Rows);
			Data->SetNumberField(TEXT("matched"), Matched.Num());
			Data->SetNumberField(TEXT("lastSeq"), (double)LastSeq);
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("settings_get"),
		TEXT("Read project or editor settings from a settings class (e.g. RendererSettings, GameMapsSettings, or your own UDeveloperSettings)."),
		TEXT(R"({"type":"object","properties":{
			"class":{"type":"string","description":"Settings class: short name or /Script/Module.Class."},
			"properties":{"type":"array","items":{"type":"string"}}},
			"required":["class"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			TArray<FString> Sug;
			UClass* C = FindSettingsClass(EW::Str(Args, TEXT("class")), Sug);
			if (!C) { return FEWResult::Error(TEXT("UNKNOWN_SETTINGS"), TEXT("No config settings class by that name."), Sug); }
			TArray<FString> Wanted;
			const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
			if (Args->TryGetArrayField(TEXT("properties"), List)) { for (const auto& V : *List) { Wanted.Add(V->AsString()); } }
			TSharedPtr<FJsonObject> Values;
			UObject* CDO = C->GetDefaultObject();
			FEWResult Res = EW::ReadValues(C, CDO, Wanted, Values);
			if (!Res.bOk) { return Res; }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("class"), C->GetPathName());
			Data->SetStringField(TEXT("configFile"), CDO->GetDefaultConfigFilename());
			Data->SetObjectField(TEXT("values"), Values);
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("settings_set"),
		TEXT("Change project settings through the running editor and write them to the project's Default*.ini, so the editor will not overwrite them on exit. Values are checked like object_set. Some settings need an editor restart."),
		TEXT(R"({"type":"object","properties":{
			"class":{"type":"string"},
			"values":{"type":"object"}},
			"required":["class","values"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			TArray<FString> Sug;
			UClass* C = FindSettingsClass(EW::Str(Args, TEXT("class")), Sug);
			if (!C) { return FEWResult::Error(TEXT("UNKNOWN_SETTINGS"), TEXT("No config settings class by that name."), Sug); }
			UObject* CDO = C->GetDefaultObject();
			const TSharedPtr<FJsonObject>* Values = nullptr;
			Args->TryGetObjectField(TEXT("values"), Values);
			FEWResult Res = EW::WriteValues(C, CDO, CDO, Values ? *Values : nullptr);
			if (!Res.bOk) { return Res; }
			bool bWritten = false;
			if (C->HasAnyClassFlags(CLASS_DefaultConfig))
			{
				bWritten = CDO->TryUpdateDefaultConfigFile();
			}
			else
			{
				CDO->SaveConfig();
				bWritten = true;
			}
			if (!bWritten) { Res.Warnings.Add(TEXT("The ini file could not be written (read-only or source control?).")); }
			Res.Data->SetStringField(TEXT("configFile"), CDO->GetDefaultConfigFilename());
			Res.Data->SetBoolField(TEXT("written"), bWritten);
			return Res;
		}, /*bTransactional*/ false);

	R.Register(TEXT("doctor"),
		TEXT("Health check of the editor and this server: Play state, unsaved packages, a slow Zen startup cause, and what to do about each."),
		TEXT(R"({"type":"object","properties":{}})"),
		false,
		[](const TSharedPtr<FJsonObject>&) -> FEWResult
		{
			FEWResult Res = FEWResult::Ok();
			TSharedRef<FJsonObject> Data = EW::EditorState();
			Data->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());

			TArray<UPackage*> Dirty;
			FEditorFileUtils::GetDirtyContentPackages(Dirty);
			FEditorFileUtils::GetDirtyWorldPackages(Dirty);
			Data->SetNumberField(TEXT("unsavedPackages"), Dirty.Num());
			if (Dirty.Num()) { Res.Warnings.Add(FString::Printf(TEXT("%d unsaved package(s). Fix: asset_save with no paths."), Dirty.Num())); }

			FString ZenDir;
			const int32 Sessions = CountZenSessions(ZenDir);
			Data->SetNumberField(TEXT("zenSessions"), Sessions);
			if (Sessions > 100)
			{
				Res.Warnings.Add(FString::Printf(TEXT("%d stored Zen sessions slow the next editor start (a 'Wait for ZenServer?' prompt). Fix: with the editor and zenserver closed, empty %s."), Sessions, *ZenDir));
			}
			if (EW::IsPlaying()) { Res.Warnings.Add(TEXT("Play In Editor runs: asset edits are blocked until pie_stop.")); }
			Data->SetBoolField(TEXT("healthy"), Res.Warnings.Num() == 0);
			Res.Data = Data;
			return Res;
		});
}
