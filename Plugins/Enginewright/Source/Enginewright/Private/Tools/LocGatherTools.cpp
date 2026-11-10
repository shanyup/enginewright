// loc_gather / loc_gather_status: run the localization steps of a target (gather, export, import, compile) with the
// editor's GatherText commandlet, one process per step, and check the result (.po files and compiled .locres). Runs inside
// the editor without blocking it; follows game_run: an id, loc_gather_status, one summary file per run (ER-11).
#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EWLocPo.h"

#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Internationalization/Regex.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"

using namespace EWLoc;

namespace
{
	FString Fwd(const FString& P) { return P.Replace(TEXT("\\"), TEXT("/")); }

	TArray<TSharedPtr<FJsonValue>> JStrings(const TArray<FString>& In, int32 Max = MAX_int32, int32 MaxLen = 500)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& S : In) { if (Out.Num() >= Max) { break; } Out.Add(MakeShared<FJsonValueString>(S.Left(MaxLen))); }
		return Out;
	}

	/** Reads a log file another process is still writing (shared read). Handles UTF-8 and UTF-16. */
	bool ReadShared(const FString& Path, FString& Out, int64& InOutSize)
	{
		TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileReader(*Path, FILEREAD_AllowWrite | FILEREAD_Silent));
		if (!Ar) { return false; }
		const int64 Size = Ar->TotalSize();
		if (Size == InOutSize) { return false; }
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized((int32)Size);
		Ar->Serialize(Bytes.GetData(), Size);
		InOutSize = Size;
		FFileHelper::BufferToString(Out, Bytes.GetData(), Bytes.Num());
		return true;
	}

	bool IsCrashLine(const FString& L)
	{
		return L.Contains(TEXT("Fatal error")) || L.Contains(TEXT("Unhandled Exception")) || L.Contains(TEXT("=== Critical error ===")) ||
			L.Contains(TEXT("Assertion failed")) || L.Contains(TEXT("appError called"));
	}

	const TCHAR* StepSuffix(const FString& Step)
	{
		if (Step == TEXT("gather")) { return TEXT("Gather"); }
		if (Step == TEXT("export")) { return TEXT("Export"); }
		if (Step == TEXT("import")) { return TEXT("Import"); }
		if (Step == TEXT("compile")) { return TEXT("Compile"); }
		return nullptr;
	}

	struct FLocRun
	{
		FString Id, Target, ConfigDir, Out, Exe;
		TArray<FString> Steps, Cultures;
		double Timeout = 900;
		int32 Index = 0;
		bool bFinished = false, bProcStarted = false;
		FProcHandle Proc;
		double Started = 0, T0 = 0, LastPoll = 0;
		FString Log, Config, StopReason;
		int64 LogSize = -1;
		FString Text;
		TArray<TSharedPtr<FJsonValue>> StepResults;
		TArray<FString> Warnings, Skipped;
		FString Verdict = TEXT("PASSED");
		TSharedPtr<FJsonObject> Result, Check;
		struct FWaiter { double Deadline; FEWDone Done; };
		TArray<FWaiter> Waiters;
	};

	TMap<FString, TSharedPtr<FLocRun>>& Runs()
	{
		static TMap<FString, TSharedPtr<FLocRun>> M;
		return M;
	}
	FTSTicker::FDelegateHandle GTick;

	/** The project folder as an absolute path (FPaths::ProjectDir() can be relative to the engine binaries). */
	FString ProjectAbs() { return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()); }

	FString ConfigPath(const FLocRun& R, const FString& Step)
	{
		return FPaths::ConvertRelativePathToFull(ProjectAbs(), R.ConfigDir / (R.Target + TEXT("_") + StepSuffix(Step) + TEXT(".ini")));
	}

	/** The UnrealEditor-Cmd.exe next to the running editor (no window, no UI), else the editor exe itself. */
	FString CommandletExe()
	{
		const FString Editor = FPaths::ConvertRelativePathToFull(FPlatformProcess::ExecutablePath());
		const FString Cmd = Editor.Replace(TEXT("UnrealEditor.exe"), TEXT("UnrealEditor-Cmd.exe"));
		return (Cmd != Editor && IFileManager::Get().FileExists(*Cmd)) ? Cmd : Editor;
	}

	FString CommandLine(const FLocRun& R, const FString& ConfigFile, const FString& LogFile)
	{
		return FString::Printf(TEXT("\"%s\" -run=GatherText -config=\"%s\" -abslog=\"%s\" -log -unattended -nosplash -nopause -nop4 -NullRHI -NoSound -SkipNestedMacroPrepass"),
			*Fwd(FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath())), *Fwd(ConfigFile), *Fwd(LogFile));
	}

	/** Path from a localization config: %LOCPROJECTROOT% and relative paths start at the project folder. */
	FString ConfigDirPath(FString P)
	{
		P.ReplaceInline(TEXT("%LOCPROJECTROOT%"), *ProjectAbs());
		return FPaths::ConvertRelativePathToFull(ProjectAbs(), P);
	}

	/** The check step: the .po files and compiled .locres files of every culture of the target. */
	TSharedRef<FJsonObject> CheckTarget(const FLocRun& R)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		FConfigFile Ini;
		for (const TCHAR* S : { TEXT("compile"), TEXT("gather"), TEXT("export"), TEXT("import") })
		{
			const FString P = ConfigPath(R, S);
			if (IFileManager::Get().FileExists(*P)) { Ini.Read(P); break; }
		}
		auto Get = [&Ini](const TCHAR* Key, const FString& Default)
		{
			FString V;
			return Ini.GetString(TEXT("CommonSettings"), Key, V) && !V.IsEmpty() ? V : Default;
		};
		const FString Dest = ConfigDirPath(Get(TEXT("DestinationPath"), Get(TEXT("SourcePath"), TEXT("Content/Localization/") + R.Target)));
		const FString Native = Get(TEXT("NativeCulture"), TEXT("en"));
		const FString PoName = Get(TEXT("PortableObjectName"), R.Target + TEXT(".po"));
		const FString ResName = Get(TEXT("ResourceName"), R.Target + TEXT(".locres"));
		const FString ManifestName = Get(TEXT("ManifestName"), R.Target + TEXT(".manifest"));
		TArray<FString> Cultures;
		Ini.GetArray(TEXT("CommonSettings"), TEXT("CulturesToGenerate"), Cultures);
		if (R.Cultures.Num()) { Cultures = R.Cultures; }
		if (Cultures.Num() == 0) { Cultures.Add(Native); }

		J->SetStringField(TEXT("target"), R.Target);
		J->SetStringField(TEXT("destination"), Fwd(Dest));
		J->SetStringField(TEXT("nativeCulture"), Native);
		const FString Manifest = Dest / ManifestName;
		J->SetBoolField(TEXT("manifestExists"), IFileManager::Get().FileExists(*Manifest));
		TArray<FString> Issues;
		if (!IFileManager::Get().FileExists(*Manifest)) { Issues.Add(FString::Printf(TEXT("%s is missing (run the gather step)."), *ManifestName)); }
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& C : Cultures)
		{
			TSharedRef<FJsonObject> CJ = MakeShared<FJsonObject>();
			CJ->SetStringField(TEXT("culture"), C);
			const bool bNative = C == Native;
			CJ->SetBoolField(TEXT("native"), bNative);
			const FString Po = Dest / C / PoName, Res = Dest / C / ResName;
			TSharedRef<FJsonObject> PJ = MakeShared<FJsonObject>();
			const bool bPo = IFileManager::Get().FileExists(*Po);
			PJ->SetBoolField(TEXT("exists"), bPo);
			if (bPo)
			{
				FString Text;
				FFileHelper::LoadFileToString(Text, *Po);
				TArray<FPoItem> Items;
				ParsePo(Text, Items);
				int32 N = 0, Empty = 0, Same = 0, Fuzzy = 0, Format = 0;
				for (const FPoItem& It : Items)
				{
					if (It.Ctxt.IsEmpty() && It.Id.IsEmpty()) { continue; }
					++N;
					if (It.bFuzzy) { ++Fuzzy; }
					if (It.Str.TrimStartAndEnd().IsEmpty()) { if (!bNative && !It.Id.TrimStartAndEnd().IsEmpty()) { ++Empty; } continue; }
					if (!bNative && It.Str == It.Id && It.Id.Len() > 1 && !It.Id.IsNumeric()) { ++Same; }
					if (Tokens(It.Id) != Tokens(It.Str)) { ++Format; }
				}
				PJ->SetNumberField(TEXT("entries"), N);
				PJ->SetNumberField(TEXT("untranslated"), Empty);
				PJ->SetNumberField(TEXT("sameAsSource"), Same);
				PJ->SetNumberField(TEXT("fuzzy"), Fuzzy);
				PJ->SetNumberField(TEXT("formatMismatch"), Format);
				if (Empty) { Issues.Add(FString::Printf(TEXT("%s: %d untranslated (empty) entries in %s."), *C, Empty, *PoName)); }
				if (Format) { Issues.Add(FString::Printf(TEXT("%s: %d entries whose format arguments / rich text tags differ from the source."), *C, Format)); }
				if (Fuzzy) { Issues.Add(FString::Printf(TEXT("%s: %d fuzzy entries."), *C, Fuzzy)); }
			}
			else if (!bNative) { Issues.Add(FString::Printf(TEXT("%s: %s is missing (run the export step)."), *C, *PoName)); }
			CJ->SetObjectField(TEXT("po"), PJ);
			TSharedRef<FJsonObject> LJ = MakeShared<FJsonObject>();
			const bool bRes = IFileManager::Get().FileExists(*Res);
			LJ->SetBoolField(TEXT("exists"), bRes);
			if (bRes)
			{
				LJ->SetNumberField(TEXT("bytes"), (double)IFileManager::Get().FileSize(*Res));
				const bool bOld = bPo && IFileManager::Get().GetTimeStamp(*Res) + FTimespan::FromSeconds(1) < IFileManager::Get().GetTimeStamp(*Po);
				LJ->SetBoolField(TEXT("olderThanPo"), bOld);
				if (bOld) { Issues.Add(FString::Printf(TEXT("%s: %s is older than %s (run the compile step)."), *C, *ResName, *PoName)); }
			}
			else { Issues.Add(FString::Printf(TEXT("%s: %s is missing (run the compile step)."), *C, *ResName)); }
			CJ->SetObjectField(TEXT("locres"), LJ);
			Out.Add(MakeShared<FJsonValueObject>(CJ));
		}
		J->SetArrayField(TEXT("cultures"), Out);
		J->SetArrayField(TEXT("issues"), JStrings(Issues));
		J->SetBoolField(TEXT("clean"), Issues.Num() == 0);
		return J;
	}

	void SetVerdict(FLocRun& R, const FString& V) { if (R.Verdict == TEXT("PASSED") || R.Verdict == TEXT("ISSUES")) { R.Verdict = V; } }

	/** Starts the commandlet of the current step. */
	bool StartStep(FLocRun& R, FString& Error)
	{
		const FString& Step = R.Steps[R.Index];
		R.Config = ConfigPath(R, Step);
		R.Log = R.Out / (FString::Printf(TEXT("%02d-"), R.Index + 1) + Step + TEXT(".log"));
		IFileManager::Get().MakeDirectory(*R.Out, true);
		IFileManager::Get().Delete(*R.Log, false, true, true);
		R.T0 = FPlatformTime::Seconds();
		R.Proc = FPlatformProcess::CreateProc(*R.Exe, *CommandLine(R, R.Config, R.Log), /*bLaunchDetached*/ false, /*bLaunchHidden*/ true,
			/*bLaunchReallyHidden*/ true, nullptr, 0, nullptr, nullptr);
		if (!R.Proc.IsValid()) { Error = FString::Printf(TEXT("could not start %s"), *R.Exe); return false; }
		R.bProcStarted = true;
		R.LogSize = -1;
		R.Text.Reset();
		return true;
	}

	void ReportStep(FLocRun& R, const FString& Step, bool bExited, int32 Code, const FString& Forced)
	{
		if (R.bProcStarted)
		{
			if (!bExited)
			{
				FPlatformProcess::TerminateProc(R.Proc, true);
				for (int32 i = 0; i < 100 && FPlatformProcess::IsProcRunning(R.Proc); ++i) { FPlatformProcess::Sleep(0.1f); }
				FPlatformProcess::Sleep(0.3f);
			}
			FPlatformProcess::CloseProc(R.Proc);
			R.bProcStarted = false;
		}
		R.LogSize = -1;
		ReadShared(R.Log, R.Text, R.LogSize);
		TArray<FString> Lines;
		R.Text.ParseIntoArrayLines(Lines, false);
		const FRegexPattern Done(TEXT("GatherText completed with exit code (-?[0-9]+)"));
		int32 Reported = INDEX_NONE, Warn = 0, Err = 0;
		bool bCrash = false, bPortError = false;
		TArray<FString> Errors, Notes;
		for (const FString& L : Lines)
		{
			FRegexMatcher M(Done, L);
			if (M.FindNext()) { Reported = FCString::Atoi(*M.GetCaptureGroup(1)); }
			if (IsCrashLine(L)) { bCrash = true; }
			if (L.Contains(TEXT(": Error:"))) { ++Err; if (Errors.Num() < 20) { Errors.Add(L); } }
			else if (L.Contains(TEXT(": Warning:"))) { ++Warn; }
			if (L.Contains(TEXT("could not listen on port")) || (L.Contains(TEXT("Port ")) && L.Contains(TEXT("is in use")))) { bPortError = true; }
		}
		FString Verdict = Forced;
		if (Verdict.IsEmpty())
		{
			// GatherText logs "exit code 0" even when a step failed (an unknown commandlet class, say), and an old Enginewright
			// inside the commandlet made the process exit with 1 on a port clash although all went well. So: both codes must be 0,
			// except that a port clash with a logged 0 still passes.
			const bool bPortOnly = Reported == 0 && bPortError;
			if (bCrash) { Verdict = TEXT("CRASHED"); }
			else if (Reported != INDEX_NONE && Reported != 0) { Verdict = TEXT("FAILED"); }
			else if (bExited && Code != 0 && !bPortOnly) { Verdict = TEXT("FAILED"); }
			else if (Reported == INDEX_NONE) { Verdict = TEXT("NO_VERDICT"); Notes.Add(TEXT("The log has no 'GatherText completed with exit code' line; see the log file.")); }
			else { Verdict = TEXT("PASSED"); }
		}
		if (Reported != INDEX_NONE && bExited && Reported != Code)
		{
			Notes.Add(FString::Printf(TEXT("Process exit code %d, but the commandlet logged exit code %d.%s"), Code, Reported,
				bPortError ? TEXT(" The log also shows a port problem (an Enginewright server started inside the commandlet); that case counts as passed.") : (Err > 0 ? TEXT(" Look at errors.") : TEXT(""))));
		}
		TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetStringField(TEXT("step"), Step);
		S->SetStringField(TEXT("config"), Fwd(R.Config));
		S->SetStringField(TEXT("log"), Fwd(R.Log));
		S->SetStringField(TEXT("verdict"), Verdict);
		if (bExited) { S->SetNumberField(TEXT("exitCode"), Code); } else { S->SetField(TEXT("exitCode"), MakeShared<FJsonValueNull>()); }
		if (Reported != INDEX_NONE) { S->SetNumberField(TEXT("reportedExitCode"), Reported); } else { S->SetField(TEXT("reportedExitCode"), MakeShared<FJsonValueNull>()); }
		S->SetNumberField(TEXT("seconds"), FMath::RoundToDouble((FPlatformTime::Seconds() - R.T0) * 10) / 10);
		S->SetNumberField(TEXT("errorCount"), Err);
		S->SetNumberField(TEXT("warningCount"), Warn);
		S->SetArrayField(TEXT("errors"), JStrings(Errors));
		if (Notes.Num()) { S->SetArrayField(TEXT("notes"), JStrings(Notes)); for (const FString& N : Notes) { R.Warnings.Add(Step + TEXT(": ") + N); } }
		R.StepResults.Add(MakeShared<FJsonValueObject>(S));
		if (Verdict != TEXT("PASSED")) { SetVerdict(R, Verdict); R.StopReason = Step; }
	}

	void Complete(FLocRun& R)
	{
		R.bFinished = true;
		for (int32 i = R.Index + 1; i < R.Steps.Num(); ++i) { R.Skipped.Add(R.Steps[i]); }
		R.Result = MakeShared<FJsonObject>();
		R.Result->SetStringField(TEXT("id"), R.Id);
		R.Result->SetStringField(TEXT("state"), TEXT("finished"));
		R.Result->SetStringField(TEXT("target"), R.Target);
		R.Result->SetStringField(TEXT("verdict"), R.Verdict);
		R.Result->SetStringField(TEXT("out"), Fwd(R.Out));
		R.Result->SetArrayField(TEXT("steps"), R.StepResults);
		if (R.Check.IsValid()) { R.Result->SetObjectField(TEXT("check"), R.Check); }
		if (R.Skipped.Num()) { R.Result->SetArrayField(TEXT("skipped"), JStrings(R.Skipped)); }
		R.Result->SetNumberField(TEXT("seconds"), FMath::RoundToInt(FPlatformTime::Seconds() - R.Started));
		const FString Summary = R.Out / TEXT("summary.json");
		IFileManager::Get().MakeDirectory(*R.Out, true);
		FFileHelper::SaveStringToFile(EW::ToJsonString(R.Result.ToSharedRef()), *Summary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		R.Result->SetStringField(TEXT("summary"), Fwd(Summary));
	}

	FEWResult FinalResult(const FLocRun& R)
	{
		FEWResult Res = FEWResult::Ok(R.Result);
		Res.Warnings = R.Warnings;
		if (R.Verdict != TEXT("PASSED") && R.Verdict != TEXT("ISSUES"))
		{
			Res = FEWResult::Error(TEXT("RUN_NOT_PASSED"), FString::Printf(TEXT("Step '%s' did not pass (%s); see data.steps for its exit codes, errors and log file. Later steps were skipped."), *R.StopReason, *R.Verdict));
			Res.Data = R.Result;
			Res.Warnings = R.Warnings;
		}
		return Res;
	}

	/** Answers the callers waiting for a finished run. */
	void Flush(FLocRun& R)
	{
		TArray<FLocRun::FWaiter> W = MoveTemp(R.Waiters);
		for (FLocRun::FWaiter& X : W) { X.Done(FinalResult(R)); }
	}

	FEWResult RunningResult(const FLocRun& R)
	{
		TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
		D->SetStringField(TEXT("id"), R.Id);
		D->SetStringField(TEXT("state"), TEXT("running"));
		D->SetStringField(TEXT("current"), R.Steps.IsValidIndex(R.Index) ? R.Steps[R.Index] : FString());
		D->SetNumberField(TEXT("finishedSteps"), R.StepResults.Num());
		D->SetNumberField(TEXT("totalSteps"), R.Steps.Num());
		D->SetNumberField(TEXT("seconds"), FMath::RoundToInt(FPlatformTime::Seconds() - R.Started));
		D->SetStringField(TEXT("out"), Fwd(R.Out));
		FEWResult Res = FEWResult::Error(TEXT("STILL_RUNNING"),
			FString::Printf(TEXT("The localization run is still going (step %s). It keeps running; call loc_gather_status {\"id\":\"%s\",\"wait\":300} for the result."), *D->GetStringField(TEXT("current")), *R.Id));
		Res.Data = D;
		return Res;
	}

	/** One step of an active run. */
	void Step(FLocRun& R)
	{
		if (R.bFinished) { return; }
		const FString StepName = R.Steps[R.Index];
		if (StepName == TEXT("check"))
		{
			R.Check = CheckTarget(R);
			TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
			const bool bClean = R.Check->GetBoolField(TEXT("clean"));
			S->SetStringField(TEXT("step"), TEXT("check"));
			S->SetStringField(TEXT("verdict"), bClean ? TEXT("PASSED") : TEXT("ISSUES"));
			R.StepResults.Add(MakeShared<FJsonValueObject>(S));
			if (!bClean) { SetVerdict(R, TEXT("ISSUES")); R.Warnings.Add(TEXT("check: ") + FString::Join(([&R]() { TArray<FString> I; for (const TSharedPtr<FJsonValue>& V : R.Check->GetArrayField(TEXT("issues"))) { I.Add(V->AsString()); } return I; })(), TEXT(" "))); }
			++R.Index;
			if (R.Index >= R.Steps.Num()) { Complete(R); }
			return;
		}
		if (!R.bProcStarted)
		{
			FString Err;
			if (!StartStep(R, Err))
			{
				TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
				S->SetStringField(TEXT("step"), StepName);
				S->SetStringField(TEXT("verdict"), TEXT("LAUNCH_FAILED"));
				S->SetStringField(TEXT("message"), Err);
				R.StepResults.Add(MakeShared<FJsonValueObject>(S));
				SetVerdict(R, TEXT("LAUNCH_FAILED"));
				R.StopReason = StepName;
				Complete(R);
				return;
			}
		}
		const double Now = FPlatformTime::Seconds();
		if (Now - R.LastPoll < 0.5) { return; }
		R.LastPoll = Now;
		const bool bExited = !FPlatformProcess::IsProcRunning(R.Proc);
		const bool bTimedOut = !bExited && Now - R.T0 > R.Timeout;
		if (!bExited && !bTimedOut) { return; }
		int32 Code = -1;
		if (bExited) { FPlatformProcess::GetProcReturnCode(R.Proc, &Code); }
		ReportStep(R, StepName, bExited, Code, bTimedOut ? TEXT("TIMEOUT") : FString());
		if (!R.StopReason.IsEmpty()) { Complete(R); return; }
		++R.Index;
		if (R.Index >= R.Steps.Num()) { Complete(R); }
	}

	bool Tick(float)
	{
		const double Now = FPlatformTime::Seconds();
		bool bActive = false;
		for (auto& KV : Runs())
		{
			FLocRun& R = *KV.Value;
			if (R.bFinished) { Flush(R); continue; }
			// A check step needs no process: loop until the next process step or the end.
			while (!R.bFinished && R.Steps.IsValidIndex(R.Index) && R.Steps[R.Index] == TEXT("check")) { Step(R); }
			if (!R.bFinished) { Step(R); }
			if (R.bFinished) { Flush(R); continue; }
			bActive = true;
			for (int32 i = R.Waiters.Num() - 1; i >= 0; --i)
			{
				if (Now > R.Waiters[i].Deadline)
				{
					FEWDone D = MoveTemp(R.Waiters[i].Done);
					R.Waiters.RemoveAt(i);
					D(RunningResult(R));
				}
			}
		}
		if (!bActive) { GTick.Reset(); }
		return bActive;
	}

	void EnsureTicker()
	{
		if (!GTick.IsValid()) { GTick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick), 0.0f); }
	}

	TArray<FString> KnownTargets(const FString& ConfigDir)
	{
		TArray<FString> Found, Targets;
		IFileManager::Get().FindFiles(Found, *(FPaths::ConvertRelativePathToFull(ProjectAbs(), ConfigDir) / TEXT("*_Gather.ini")), true, false);
		for (const FString& F : Found) { Targets.Add(F.LeftChop(FString(TEXT("_Gather.ini")).Len())); }
		return Targets;
	}
}

void EWStopLocRuns()
{
	for (auto& KV : Runs())
	{
		FLocRun& R = *KV.Value;
		if (R.bProcStarted) { FPlatformProcess::TerminateProc(R.Proc, true); FPlatformProcess::CloseProc(R.Proc); R.bProcStarted = false; }
	}
	Runs().Empty();
	if (GTick.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(GTick); GTick.Reset(); }
}

void RegisterLocGatherTools(FEWRegistry& Reg)
{
	Reg.RegisterAsync(TEXT("loc_gather"),
		TEXT("Run the localization steps of a target with the GatherText commandlet (the Localization Dashboard's Gather, Export, Import and Compile buttons) and check the result, in one call: steps gather, export (writes the .po files), import (reads them back), compile (writes the .locres) and check (the .po files and .locres files: untranslated and fuzzy entries, format argument differences, a .locres older than its .po). ")
		TEXT("Default steps: gather, compile, check. Each commandlet step is its own UnrealEditor-Cmd process, so it works while this editor is open; the commandlet does not start an Enginewright server (no port clash). A step passes when the process exit code and the code in its log line 'GatherText completed with exit code N' are both 0 (GatherText logs 0 even when a step failed, so the process code counts; both are reported with the error lines). ")
		TEXT("The configs are the dashboard's: <configDir>/<target>_Gather.ini, _Export.ini, _Import.ini, _Compile.ini (default Config/Localization). The run stops at the first step that does not pass. Output: <project>/Saved/Enginewright/loc/run-<time>/ (summary.json, one log per step). ")
		TEXT("If the result is not ready within 'wait' seconds the reply is STILL_RUNNING with an id: follow it with loc_gather_status. This writes files under Content/Localization (and the .po files); it does not change the open level. To compare a .po with the source text use stringtable_compare; to preview a language use loc_culture and game_run's culture."),
		TEXT(R"({"type":"object","properties":{
			"target":{"type":"string","description":"Localization target name, e.g. Game (the prefix of Game_Gather.ini)."},
			"steps":{"type":"array","items":{"type":"string","enum":["gather","export","import","compile","check"]},"description":"Steps in order, each at most once. Default [\"gather\",\"compile\",\"check\"]."},
			"cultures":{"type":"array","items":{"type":"string"},"description":"Cultures the check step looks at. Default: the CulturesToGenerate of the target's config."},
			"configDir":{"type":"string","description":"Folder of the target's .ini files, relative to the project folder (default Config/Localization)."},
			"timeout":{"type":"number","description":"Seconds per commandlet step before it is stopped (default 900)."},
			"wait":{"type":"number","description":"Seconds to wait for the result before replying STILL_RUNNING (default 300; 0 = reply at once with the id)."},
			"dryRun":{"type":"boolean","description":"Return the commands and config files only; start nothing."}},
			"required":["target"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args, FEWDone Done)
		{
			for (const auto& KV : Runs())
			{
				if (!KV.Value->bFinished)
				{
					Done(FEWResult::Error(TEXT("BUSY"), FString::Printf(TEXT("Localization run %s is still going; follow it with loc_gather_status, or stop it with loc_gather_status {\"id\":\"%s\",\"stop\":true}."), *KV.Key, *KV.Key)));
					return;
				}
			}
			TSharedPtr<FLocRun> R = MakeShared<FLocRun>();
			R->Target = EW::Str(Args, TEXT("target")).TrimStartAndEnd();
			R->ConfigDir = EW::Str(Args, TEXT("configDir"));
			if (R->ConfigDir.IsEmpty()) { R->ConfigDir = TEXT("Config/Localization"); }
			R->Steps = EW::StrList(Args, TEXT("steps"));
			R->Cultures = EW::StrList(Args, TEXT("cultures"));
			R->Timeout = EW::Num(Args, TEXT("timeout"), 900);
			const double Wait = EW::Num(Args, TEXT("wait"), 300);
			if (R->Target.IsEmpty() || R->Target.Contains(TEXT("/")) || R->Target.Contains(TEXT("\\"))) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("target must be a target name such as Game, not a path."))); return; }
			if (R->Timeout <= 0 || Wait < 0) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("timeout must be above 0 and wait 0 or more."))); return; }
			if (R->Steps.IsEmpty()) { R->Steps = { TEXT("gather"), TEXT("compile"), TEXT("check") }; }
			const TArray<FString> Valid = { TEXT("gather"), TEXT("export"), TEXT("import"), TEXT("compile"), TEXT("check") };
			TSet<FString> Seen;
			for (const FString& S : R->Steps)
			{
				if (!Valid.Contains(S)) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Unknown step '%s'."), *S), Valid)); return; }
				if (Seen.Contains(S)) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Step '%s' is listed twice."), *S))); return; }
				Seen.Add(S);
			}
			// Every config must exist before anything starts.
			TSharedRef<FJsonObject> Configs = MakeShared<FJsonObject>();
			bool bAnyConfig = false;
			for (const FString& S : R->Steps)
			{
				if (S == TEXT("check")) { continue; }
				const FString P = ConfigPath(*R, S);
				if (!IFileManager::Get().FileExists(*P))
				{
					Done(FEWResult::Error(TEXT("CONFIG_NOT_FOUND"), FString::Printf(TEXT("No config for step '%s': %s does not exist. The Localization Dashboard writes <target>_Gather.ini, _Export.ini, _Import.ini and _Compile.ini; check target and configDir."), *S, *Fwd(P)), KnownTargets(R->ConfigDir)));
					return;
				}
				Configs->SetStringField(S, Fwd(P));
				bAnyConfig = true;
			}
			if (Seen.Contains(TEXT("check")) && !bAnyConfig)
			{
				bool bFound = false;
				for (const TCHAR* S : { TEXT("compile"), TEXT("gather"), TEXT("export"), TEXT("import") }) { bFound |= IFileManager::Get().FileExists(*ConfigPath(*R, S)); }
				if (!bFound) { Done(FEWResult::Error(TEXT("CONFIG_NOT_FOUND"), FString::Printf(TEXT("The check step reads the target's config, but no %s_*.ini exists in %s."), *R->Target, *R->ConfigDir), KnownTargets(R->ConfigDir))); return; }
			}
			R->Id = FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
			R->Out = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Enginewright/loc") / (TEXT("run-") + R->Id));
			FPaths::NormalizeDirectoryName(R->Out);
			R->Exe = CommandletExe();
			if (EW::Bool(Args, TEXT("dryRun"), false))
			{
				TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> Cmds;
				for (const FString& S : R->Steps)
				{
					if (S == TEXT("check")) { continue; }
					Cmds.Add(MakeShared<FJsonValueString>(Fwd(R->Exe) + TEXT(" ") + CommandLine(*R, ConfigPath(*R, S), R->Out / (S + TEXT(".log")))));
				}
				D->SetArrayField(TEXT("commands"), Cmds);
				D->SetObjectField(TEXT("configs"), Configs);
				D->SetArrayField(TEXT("steps"), JStrings(R->Steps));
				D->SetStringField(TEXT("out"), Fwd(R->Out));
				Done(FEWResult::Ok(D, TEXT("Dry run: nothing was started.")));
				return;
			}
			R->Started = FPlatformTime::Seconds();
			R->Waiters.Add({ R->Started + Wait, MoveTemp(Done) });
			Runs().Add(R->Id, R);
			EnsureTicker();
			if (Wait <= 0)
			{
				FEWDone D = MoveTemp(R->Waiters.Last().Done);
				R->Waiters.Pop();
				Step(*R); // starts the first step now
				D(RunningResult(*R));
				return;
			}
			Step(*R);
		});

	Reg.RegisterAsync(TEXT("loc_gather_status"),
		TEXT("Result of a loc_gather: waits up to 'wait' seconds for it to finish, then gives the same report as loc_gather (or STILL_RUNNING). stop true ends the running commandlet now. Without id: the latest run."),
		TEXT(R"({"type":"object","properties":{
			"id":{"type":"string","description":"Run id from loc_gather (default: the latest)."},
			"wait":{"type":"number","description":"Seconds to wait for the result (default 0)."},
			"stop":{"type":"boolean","description":"Stop the running commandlet now (the run ends with verdict STOPPED)."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args, FEWDone Done)
		{
			FString Id = EW::Str(Args, TEXT("id"));
			TArray<FString> Keys;
			Runs().GetKeys(Keys);
			Keys.Sort();
			if (Id.IsEmpty())
			{
				if (Keys.Num() == 0) { Done(FEWResult::Error(TEXT("NO_RUN"), TEXT("No loc_gather in this editor session."))); return; }
				Id = Keys.Last();
			}
			TSharedPtr<FLocRun>* Found = Runs().Find(Id);
			if (!Found) { Done(FEWResult::Error(TEXT("NO_RUN"), FString::Printf(TEXT("No localization run with id '%s' in this editor session."), *Id), EW::Suggest(Id, Keys))); return; }
			FLocRun& R = **Found;
			if (!R.bFinished && EW::Bool(Args, TEXT("stop"), false))
			{
				if (R.bProcStarted) { ReportStep(R, R.Steps[R.Index], false, -1, TEXT("STOPPED")); }
				else { SetVerdict(R, TEXT("STOPPED")); R.StopReason = R.Steps.IsValidIndex(R.Index) ? R.Steps[R.Index] : FString(); }
				Complete(R);
				Flush(R);
			}
			if (R.bFinished) { Done(FinalResult(R)); return; }
			const double Wait = EW::Num(Args, TEXT("wait"), 0);
			if (Wait <= 0) { Done(RunningResult(R)); return; }
			R.Waiters.Add({ FPlatformTime::Seconds() + Wait, MoveTemp(Done) });
			EnsureTicker();
		});
}
