// game_run / game_run_status: start the game itself (UnrealEditor <project> <url> -game, or a packaged exe) once
// per resolution, check that the URL arrived unchanged, keep the log lines you ask for, decide PASSED/FAILED,
// and collect the PNG files it wrote (ER-02). Runs inside the editor without blocking it; no Python.
#include "EnginewrightTool.h"
#include "EWCommon.h"

#include "Components/SkyLightComponent.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "LevelEditorViewport.h"
#include "Modules/ModuleManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Regex.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Internationalization/Culture.h"
#include "Serialization/Archive.h"
#if PLATFORM_WINDOWS
#include "ILiveCodingModule.h"
#endif

namespace
{
	TArray<TSharedPtr<FJsonValue>> JStrings(const TArray<FString>& In, int32 Max = MAX_int32, int32 MaxLen = 500)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& S : In) { if (Out.Num() >= Max) { break; } Out.Add(MakeShared<FJsonValueString>(S.Left(MaxLen))); }
		return Out;
	}

	FString Fwd(const FString& P) { return P.Replace(TEXT("\\"), TEXT("/")); }

	FString QuoteArg(const FString& A)
	{
		if (!A.IsEmpty() && !A.Contains(TEXT(" ")) && !A.Contains(TEXT("\t")) && !A.Contains(TEXT("\""))) { return A; }
		FString Key, Value;
		if (A.StartsWith(TEXT("-")) && A.Split(TEXT("="), &Key, &Value) && !Key.Contains(TEXT(" ")))
		{
			return Key + TEXT("=\"") + Value.Replace(TEXT("\""), TEXT("\\\"")) + TEXT("\""); // -abslog="C:/a b/game.log"
		}
		return TEXT("\"") + A.Replace(TEXT("\""), TEXT("\\\"")) + TEXT("\"");
	}

	bool AnyMatch(const TArray<FRegexPattern>& Ps, const FString& Line)
	{
		for (const FRegexPattern& P : Ps)
		{
			FRegexMatcher M(P, Line);
			if (M.FindNext()) { return true; }
		}
		return false;
	}

	bool IsCrashLine(const FString& L)
	{
		return L.Contains(TEXT("Fatal error")) || L.Contains(TEXT("Unhandled Exception")) || L.Contains(TEXT("=== Critical error ===")) ||
			L.Contains(TEXT("Assertion failed")) || L.Contains(TEXT("appError called"));
	}

	/** Reads a log file another process is still writing (shared read). Handles UTF-8 and UTF-16. */
	bool ReadShared(const FString& Path, FString& Out, int64& InOutSize)
	{
		TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileReader(*Path, FILEREAD_AllowWrite | FILEREAD_Silent));
		if (!Ar) { return false; }
		const int64 Size = Ar->TotalSize();
		if (Size == InOutSize) { return false; } // unchanged
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized((int32)Size);
		Ar->Serialize(Bytes.GetData(), Size);
		InOutSize = Size;
		FFileHelper::BufferToString(Out, Bytes.GetData(), Bytes.Num());
		return true;
	}

	bool PngSize(const FString& Path, int32& W, int32& H)
	{
		TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileReader(*Path, FILEREAD_AllowWrite | FILEREAD_Silent));
		if (!Ar || Ar->TotalSize() < 24) { return false; }
		uint8 B[24];
		Ar->Serialize(B, 24);
		static const uint8 Sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
		if (FMemory::Memcmp(B, Sig, 8) != 0 || FMemory::Memcmp(B + 12, "IHDR", 4) != 0) { return false; }
		W = (B[16] << 24) | (B[17] << 16) | (B[18] << 8) | B[19];
		H = (B[20] << 24) | (B[21] << 16) | (B[22] << 8) | B[23];
		return true;
	}

	struct FRunSpec
	{
		FString Url, Exe, Out, Done, Require, Culture;
		TArray<FString> ExeArgs, Args, Filters, PassText, FailText, ShotDirs;
		TArray<FIntPoint> Resolutions; // empty = one run at the default size
		bool bFullscreen = false, bUnattended = true, bPauseViewports = true;
		int32 ShotsExpected = 0; // 0 = off; else the run is over when this many new PNG files exist
		double Timeout = 600, DoneGrace = 20, RequireWithin = 120, UrlCheckTimeout = 120;
		int32 MaxLines = 300;
	};

	struct FGameRun
	{
		FString Id;
		FRunSpec S;
		TArray<FRegexPattern> Pass, Fail;
		TOptional<FRegexPattern> DoneRe, RequireRe;
		int32 Index = 0;           // current resolution
		bool bFinished = false;
		TSharedPtr<FJsonObject> Result;
		TArray<TSharedPtr<FJsonValue>> Runs;
		double Started = 0;
		// current game process
		FProcHandle Proc;
		bool bProcStarted = false;
		double T0 = 0;
		FDateTime T0Utc;
		FString Tag, RunDir, ShotsDir, Log, UrlFilled;
		TArray<FString> Command;
		TSharedPtr<FJsonObject> Cur;
		int64 LogSize = -1;
		FString Text;
		int32 LinesScanned = 0;
		TOptional<bool> UrlOk;
		FString StopReason, CommandLineSeen;
		double DoneAt = -1;
		bool bRequireSeen = false;
		double LastPoll = 0;
		// shotsExpected: new PNG files seen so far and their total size at the last check (stable size = written)
		int32 ShotsFound = 0;
		int64 ShotsBytes = -1;
		double LastShotCheck = 0;
		// editor checks before the run (ER-07, ER-08); reported with the result
		TArray<FString> Warnings;
		TSharedPtr<FJsonObject> Checks;
		TArray<FLevelEditorViewportClient*> PausedViewports;
		// callers waiting for the result
		struct FWaiter { double Deadline; FEWDone Done; };
		TArray<FWaiter> Waiters;
	};

	TMap<FString, TSharedPtr<FGameRun>>& Runs()
	{
		static TMap<FString, TSharedPtr<FGameRun>> M;
		return M;
	}
	FTSTicker::FDelegateHandle GTick;

	bool FillPlaceholders(const FString& In, const TMap<FString, FString>& Vals, FString& Out, FString& Error)
	{
		Out.Reset();
		int32 i = 0;
		while (i < In.Len())
		{
			if (In[i] == TEXT('{'))
			{
				int32 j = i + 1;
				while (j < In.Len() && (FChar::IsAlnum(In[j]) || In[j] == TEXT('_'))) { ++j; }
				if (j < In.Len() && In[j] == TEXT('}') && j > i + 1)
				{
					const FString Key = In.Mid(i + 1, j - i - 1);
					const FString* V = Vals.Find(Key);
					if (!V)
					{
						TArray<FString> Known;
						Vals.GetKeys(Known);
						Known.Sort();
						Error = FString::Printf(TEXT("unknown placeholder {%s} in '%s' (known: {%s})"), *Key, *In, *FString::Join(Known, TEXT("}, {")));
						return false;
					}
					Out += *V;
					i = j + 1;
					continue;
				}
			}
			Out.AppendChar(In[i++]);
		}
		return true;
	}

	TMap<FString, FString> PlaceholderValues(const FGameRun& R, const FIntPoint* Res)
	{
		TMap<FString, FString> V;
		V.Add(TEXT("shots"), Fwd(R.ShotsDir));
		V.Add(TEXT("out"), Fwd(R.RunDir));
		V.Add(TEXT("w"), Res ? FString::FromInt(Res->X) : FString());
		V.Add(TEXT("h"), Res ? FString::FromInt(Res->Y) : FString());
		V.Add(TEXT("res"), R.Tag);
		return V;
	}

	/** Builds the command of run Index. Returns false with Error on a bad placeholder. */
	bool PrepareRun(FGameRun& R, FString& Error)
	{
		const FIntPoint* Res = R.S.Resolutions.IsValidIndex(R.Index) ? &R.S.Resolutions[R.Index] : nullptr;
		R.Tag = Res ? FString::Printf(TEXT("%dx%d"), Res->X, Res->Y) : FString(TEXT("default"));
		R.RunDir = R.S.Out / R.Tag;
		R.ShotsDir = R.RunDir / TEXT("shots");
		R.Log = R.RunDir / TEXT("game.log");
		const TMap<FString, FString> Vals = PlaceholderValues(R, Res);
		if (!FillPlaceholders(R.S.Url, Vals, R.UrlFilled, Error)) { return false; }
		R.Command.Reset();
		if (!R.S.Exe.IsEmpty())
		{
			R.Command.Add(R.S.Exe);
			R.Command.Append(R.S.ExeArgs);
			if (!R.UrlFilled.IsEmpty()) { R.Command.Add(R.UrlFilled); }
		}
		else
		{
			R.Command.Add(FPaths::ConvertRelativePathToFull(FPlatformProcess::ExecutablePath()));
			R.Command.Add(FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
			if (!R.UrlFilled.IsEmpty()) { R.Command.Add(R.UrlFilled); }
			R.Command.Add(TEXT("-game"));
		}
		if (Res)
		{
			R.Command.Add(FString::Printf(TEXT("-ResX=%d"), Res->X));
			R.Command.Add(FString::Printf(TEXT("-ResY=%d"), Res->Y));
			R.Command.Add(R.S.bFullscreen ? TEXT("-fullscreen") : TEXT("-windowed"));
		}
		R.Command.Add(TEXT("-abslog=") + Fwd(R.Log));
		R.Command.Add(TEXT("-log"));
		R.Command.Add(TEXT("-nosplash"));
		if (R.S.bUnattended) { R.Command.Add(TEXT("-unattended")); }
		if (!R.S.Culture.IsEmpty()) { R.Command.Add(TEXT("-culture=") + R.S.Culture); } // ER-12: the game is a separate process, loc_culture does not reach it
		for (const FString& A : R.S.Args)
		{
			FString Filled;
			if (!FillPlaceholders(A, Vals, Filled, Error)) { return false; }
			R.Command.Add(Filled);
		}
		R.Cur = MakeShared<FJsonObject>();
		R.Cur->SetStringField(TEXT("res"), R.Tag);
		R.Cur->SetArrayField(TEXT("command"), JStrings(R.Command, MAX_int32, 4000));
		R.Cur->SetStringField(TEXT("log"), Fwd(R.Log));
		R.Cur->SetStringField(TEXT("url"), R.UrlFilled);
		if (!R.S.Culture.IsEmpty()) { R.Cur->SetStringField(TEXT("culture"), R.S.Culture); }
		return true;
	}

	bool StartRun(FGameRun& R, FString& Error)
	{
		IFileManager::Get().MakeDirectory(*R.ShotsDir, true);
		IFileManager::Get().Delete(*R.Log, false, true, true);
		FString Params;
		for (int32 i = 1; i < R.Command.Num(); ++i) { Params += (i > 1 ? TEXT(" ") : TEXT("")) + QuoteArg(R.Command[i]); }
		R.T0 = FPlatformTime::Seconds();
		R.T0Utc = FDateTime::UtcNow();
		R.Proc = FPlatformProcess::CreateProc(*R.Command[0], *Params, /*bLaunchDetached*/ false, /*bLaunchHidden*/ false,
			/*bLaunchReallyHidden*/ false, nullptr, 0, nullptr, nullptr);
		if (!R.Proc.IsValid())
		{
			Error = FString::Printf(TEXT("could not start %s"), *R.Command[0]);
			return false;
		}
		R.bProcStarted = true;
		R.LogSize = -1;
		R.Text.Reset();
		R.LinesScanned = 0;
		R.UrlOk.Reset();
		if (R.UrlFilled.IsEmpty()) { R.UrlOk = true; }
		R.StopReason.Reset();
		R.CommandLineSeen.Reset();
		R.DoneAt = -1;
		R.bRequireSeen = false;
		R.ShotsFound = 0;
		R.ShotsBytes = -1;
		R.LastShotCheck = 0;
		return true;
	}

	/** Folders searched for new PNG files: the run's shots folder, the 'shots' folders, Saved/Screenshots for editor runs. */
	TArray<FString> ShotSearchDirs(const FGameRun& R, const FIntPoint* Res)
	{
		TArray<FString> Dirs = { R.ShotsDir };
		const TMap<FString, FString> Vals = PlaceholderValues(R, Res);
		for (const FString& D : R.S.ShotDirs)
		{
			FString Filled, Err;
			if (FillPlaceholders(D, Vals, Filled, Err)) { Dirs.Add(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Filled)); }
		}
		if (R.S.Exe.IsEmpty()) { Dirs.Add(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Screenshots"))); }
		return Dirs;
	}

	/** Counts the PNG files written since this run started (no copying) and their total size. */
	int32 CountNewPngs(const FGameRun& R, const FIntPoint* Res, int64& OutBytes)
	{
		OutBytes = 0;
		TSet<FString> Seen;
		for (const FString& D : ShotSearchDirs(R, Res))
		{
			if (!IFileManager::Get().DirectoryExists(*D)) { continue; }
			TArray<FString> Found;
			IFileManager::Get().FindFilesRecursive(Found, *D, TEXT("*.png"), true, false);
			for (const FString& F0 : Found)
			{
				const FString F = Fwd(FPaths::ConvertRelativePathToFull(F0));
				if (Seen.Contains(F) || IFileManager::Get().GetTimeStamp(*F) < R.T0Utc - FTimespan::FromSeconds(1)) { continue; }
				Seen.Add(F);
				OutBytes += FMath::Max<int64>(0, IFileManager::Get().FileSize(*F));
			}
		}
		return Seen.Num();
	}

	TArray<TSharedPtr<FJsonValue>> CollectPngs(const FGameRun& R, const FIntPoint* Res)
	{
		const TArray<FString> Dirs = ShotSearchDirs(R, Res);
		TArray<TSharedPtr<FJsonValue>> Out;
		TSet<FString> Seen;
		const FString ShotsPrefix = Fwd(FPaths::ConvertRelativePathToFull(R.ShotsDir)) + TEXT("/");
		for (const FString& D : Dirs)
		{
			if (!IFileManager::Get().DirectoryExists(*D)) { continue; }
			TArray<FString> Found;
			IFileManager::Get().FindFilesRecursive(Found, *D, TEXT("*.png"), true, false);
			Found.Sort();
			for (FString F : Found)
			{
				F = Fwd(FPaths::ConvertRelativePathToFull(F));
				if (Seen.Contains(F)) { continue; }
				const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*F);
				if (Stamp < R.T0Utc - FTimespan::FromSeconds(1)) { continue; }
				Seen.Add(F);
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				FString Final = F;
				if (!F.StartsWith(ShotsPrefix)) // copy shots written elsewhere into the run folder
				{
					FString Target = R.ShotsDir / FPaths::GetCleanFilename(F);
					for (int32 n = 1; IFileManager::Get().FileExists(*Target); ++n)
					{
						Target = R.ShotsDir / FString::Printf(TEXT("%s_%d.png"), *FPaths::GetBaseFilename(F), n);
					}
					if (IFileManager::Get().Copy(*Target, *F) == COPY_OK)
					{
						Final = Fwd(FPaths::ConvertRelativePathToFull(Target));
						Seen.Add(Final);
						Item->SetStringField(TEXT("copiedFrom"), F);
					}
				}
				Item->SetStringField(TEXT("path"), Final);
				Item->SetNumberField(TEXT("bytes"), (double)IFileManager::Get().FileSize(*Final));
				int32 W = 0, H = 0;
				if (PngSize(Final, W, H))
				{
					Item->SetNumberField(TEXT("width"), W);
					Item->SetNumberField(TEXT("height"), H);
					Item->SetBoolField(TEXT("matchesRes"), Res == nullptr || (W == Res->X && H == Res->Y));
				}
				else { Item->SetStringField(TEXT("error"), TEXT("not a PNG file")); }
				Out.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
		return Out;
	}

	/** Scans log lines that arrived since the last poll. */
	void ScanNewLines(FGameRun& R)
	{
		if (!ReadShared(R.Log, R.Text, R.LogSize)) { return; }
		TArray<FString> Lines;
		R.Text.ParseIntoArrayLines(Lines, false);
		// The last entry is either a line that may still be half written (no line break at the end) or, when the text
		// ends with a break, possibly the empty text after it. Neither counts as scanned: counting the empty entry
		// made the first line of the next write invisible to done/require (seen with HighResShot's log line).
		const bool bEndsWithBreak = R.Text.EndsWith(TEXT("\n")) || R.Text.EndsWith(TEXT("\r"));
		int32 Complete = Lines.Num();
		if (Complete > 0 && (!bEndsWithBreak || Lines.Last().IsEmpty())) { --Complete; }
		for (int32 i = R.LinesScanned; i < Complete; ++i)
		{
			const FString& L = Lines[i];
			if (!R.UrlOk.IsSet() && L.Contains(TEXT("Command Line:")))
			{
				FString After;
				L.Split(TEXT("Command Line:"), nullptr, &After);
				R.CommandLineSeen = After.TrimStartAndEnd().Left(1000);
				R.UrlOk = L.Contains(R.UrlFilled);
				if (!R.UrlOk.GetValue()) { R.StopReason = TEXT("URL_MISMATCH"); }
			}
			if (R.RequireRe.IsSet() && !R.bRequireSeen)
			{
				FRegexMatcher M(R.RequireRe.GetValue(), L);
				R.bRequireSeen = M.FindNext();
			}
			if (R.DoneRe.IsSet() && R.DoneAt < 0)
			{
				FRegexMatcher M(R.DoneRe.GetValue(), L);
				if (M.FindNext()) { R.DoneAt = FPlatformTime::Seconds(); }
			}
		}
		R.LinesScanned = FMath::Max(R.LinesScanned, Complete);
	}

	/** Ends the current game and stores its report. */
	void FinishCurrent(FGameRun& R, bool bExited, int32 ExitCode)
	{
		const bool bTimedOut = !bExited && R.StopReason.IsEmpty();
		if (!R.UrlOk.IsSet() && R.StopReason.IsEmpty()) { R.StopReason = TEXT("URL_NOT_CONFIRMED"); }
		if (!bExited)
		{
			FPlatformProcess::TerminateProc(R.Proc, /*KillTree*/ true);
			for (int32 i = 0; i < 100 && FPlatformProcess::IsProcRunning(R.Proc); ++i) { FPlatformProcess::Sleep(0.1f); }
			FPlatformProcess::Sleep(0.5f);
		}
		FPlatformProcess::CloseProc(R.Proc);
		R.bProcStarted = false;
		R.LogSize = -1;
		ScanNewLines(R);
		TArray<FString> Lines;
		R.Text.ParseIntoArrayLines(Lines, false);
		TArray<FString> Keep, Fails, Passes, Crash;
		for (const FString& L : Lines)
		{
			const bool bF = AnyMatch(R.Fail, L), bP = AnyMatch(R.Pass, L), bC = IsCrashLine(L);
			bool bFilter = false;
			for (const FString& F : R.S.Filters) { if (L.Contains(F)) { bFilter = true; break; } }
			if (bFilter || bF || bP || bC) { Keep.Add(L); }
			if (bF) { Fails.Add(L); }
			if (bP) { Passes.Add(L); }
			if (bC) { Crash.Add(L); }
		}
		FString Verdict;
		if (R.StopReason == TEXT("URL_MISMATCH") || R.StopReason == TEXT("URL_NOT_CONFIRMED") || R.StopReason == TEXT("REQUIRED_LINE_MISSING") || R.StopReason == TEXT("STOPPED")) { Verdict = R.StopReason; }
		else if (Fails.Num()) { Verdict = TEXT("FAILED"); }
		else if (Crash.Num()) { Verdict = TEXT("CRASHED"); }
		else if (R.StopReason == TEXT("SHOTS_DONE") && R.Pass.Num() == 0) { Verdict = TEXT("PASSED"); } // the expected screenshots are the result
		else if (R.Pass.Num()) { Verdict = Passes.Num() ? TEXT("PASSED") : (bTimedOut ? TEXT("TIMEOUT") : TEXT("NO_VERDICT")); }
		else { Verdict = bTimedOut ? TEXT("TIMEOUT") : (ExitCode == 0 ? TEXT("EXITED_OK") : TEXT("EXIT_CODE")); }
		const FIntPoint* Res = R.S.Resolutions.IsValidIndex(R.Index) ? &R.S.Resolutions[R.Index] : nullptr;
		TSharedPtr<FJsonObject> C = R.Cur;
		C->SetStringField(TEXT("verdict"), Verdict);
		if (bExited) { C->SetNumberField(TEXT("exitCode"), ExitCode); } else { C->SetField(TEXT("exitCode"), MakeShared<FJsonValueNull>()); }
		C->SetBoolField(TEXT("exited"), bExited);
		C->SetBoolField(TEXT("timedOut"), bTimedOut);
		if (R.StopReason.IsEmpty()) { C->SetField(TEXT("stopReason"), MakeShared<FJsonValueNull>()); } else { C->SetStringField(TEXT("stopReason"), R.StopReason); }
		C->SetNumberField(TEXT("seconds"), FMath::RoundToDouble((FPlatformTime::Seconds() - R.T0) * 10) / 10);
		if (R.UrlOk.IsSet()) { C->SetBoolField(TEXT("urlConfirmed"), R.UrlOk.GetValue()); } else { C->SetField(TEXT("urlConfirmed"), MakeShared<FJsonValueNull>()); }
		if (!R.CommandLineSeen.IsEmpty()) { C->SetStringField(TEXT("commandLineSeen"), R.CommandLineSeen); }
		C->SetArrayField(TEXT("lines"), JStrings(Keep, R.S.MaxLines));
		C->SetNumberField(TEXT("lineCount"), Keep.Num());
		C->SetArrayField(TEXT("failLines"), JStrings(Fails, 50));
		C->SetArrayField(TEXT("crashLines"), JStrings(Crash, 20));
		C->SetArrayField(TEXT("screenshots"), CollectPngs(R, Res));
		if (R.S.ShotsExpected > 0)
		{
			int64 Bytes = 0;
			C->SetNumberField(TEXT("shotsExpected"), R.S.ShotsExpected);
			C->SetNumberField(TEXT("shotsFound"), CountNewPngs(R, Res, Bytes));
		}
		if (R.Text.TrimStartAndEnd().IsEmpty()) { C->SetStringField(TEXT("warning"), TEXT("the game wrote no log; check the command (wrong exe/project?)")); }
		R.Runs.Add(MakeShared<FJsonValueObject>(C.ToSharedRef()));
	}

	FEWResult FinalResult(const FGameRun& R)
	{
		FString LaunchError;
		if (R.Result->TryGetStringField(TEXT("error"), LaunchError))
		{
			FEWResult Res = FEWResult::Error(LaunchError, R.Result->GetStringField(TEXT("message")));
			Res.Data = R.Result;
			return Res;
		}
		FEWResult Res = FEWResult::Ok(R.Result);
		Res.Warnings = R.Warnings;
		bool bAllGood = R.Runs.Num() > 0;
		for (const TSharedPtr<FJsonValue>& V : R.Runs)
		{
			const FString Verdict = V->AsObject()->GetStringField(TEXT("verdict"));
			if (Verdict != TEXT("PASSED") && Verdict != TEXT("EXITED_OK")) { bAllGood = false; }
		}
		if (!bAllGood)
		{
			Res = FEWResult::Error(TEXT("RUN_NOT_PASSED"), TEXT("At least one run did not pass; see data.verdicts and each run's failLines / crashLines / stopReason."));
			Res.Data = R.Result;
			Res.Warnings = R.Warnings;
		}
		return Res;
	}

	FEWResult RunningResult(const FGameRun& R)
	{
		TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
		D->SetStringField(TEXT("id"), R.Id);
		D->SetStringField(TEXT("state"), TEXT("running"));
		D->SetStringField(TEXT("current"), R.Tag);
		D->SetNumberField(TEXT("finishedRuns"), R.Runs.Num());
		D->SetNumberField(TEXT("totalRuns"), FMath::Max(1, R.S.Resolutions.Num()));
		D->SetNumberField(TEXT("seconds"), FMath::RoundToInt(FPlatformTime::Seconds() - R.Started));
		D->SetStringField(TEXT("out"), Fwd(R.S.Out));
		FEWResult Res = FEWResult::Error(TEXT("STILL_RUNNING"),
			FString::Printf(TEXT("The game is still running (run %s). It keeps running; call game_run_status {\"id\":\"%s\",\"wait\":300} for the result."), *R.Id, *R.Id));
		Res.Data = D;
		return Res;
	}

	const FText& PauseReason()
	{
		static const FText T = FText::FromString(TEXT("Enginewright game_run"));
		return T;
	}

	/** Gives the level viewports paused for the run their realtime state back (only viewports that still exist). */
	void RestoreViewports(FGameRun& R)
	{
		if (GEditor)
		{
			const TArray<FLevelEditorViewportClient*>& Live = GEditor->GetLevelViewportClients();
			for (FLevelEditorViewportClient* V : R.PausedViewports) { if (Live.Contains(V)) { V->RemoveRealtimeOverride(PauseReason(), false); } }
		}
		R.PausedViewports.Reset();
	}

	void Complete(FGameRun& R)
	{
		R.bFinished = true;
		RestoreViewports(R);
		if (!R.Result.IsValid()) { R.Result = MakeShared<FJsonObject>(); }
		R.Result->SetStringField(TEXT("id"), R.Id);
		R.Result->SetStringField(TEXT("state"), TEXT("finished"));
		if (R.Checks.IsValid()) { R.Result->SetObjectField(TEXT("checks"), R.Checks); }
		R.Result->SetStringField(TEXT("out"), Fwd(R.S.Out));
		R.Result->SetArrayField(TEXT("runs"), R.Runs);
		TSharedRef<FJsonObject> Verdicts = MakeShared<FJsonObject>();
		for (const TSharedPtr<FJsonValue>& V : R.Runs)
		{
			Verdicts->SetStringField(V->AsObject()->GetStringField(TEXT("res")), V->AsObject()->GetStringField(TEXT("verdict")));
		}
		R.Result->SetObjectField(TEXT("verdicts"), Verdicts);
		R.Result->SetNumberField(TEXT("seconds"), FMath::RoundToInt(FPlatformTime::Seconds() - R.Started));
		const FString Summary = R.S.Out / TEXT("summary.json");
		IFileManager::Get().MakeDirectory(*R.S.Out, true);
		FFileHelper::SaveStringToFile(EW::ToJsonString(R.Result.ToSharedRef()), *Summary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		R.Result->SetStringField(TEXT("summary"), Fwd(Summary));
		TArray<FGameRun::FWaiter> W = MoveTemp(R.Waiters);
		for (FGameRun::FWaiter& X : W) { X.Done(FinalResult(R)); }
	}

	/** One step of an active run. */
	void Step(FGameRun& R)
	{
		if (R.bFinished) { return; }
		if (!R.bProcStarted)
		{
			FString Err;
			if (!PrepareRun(R, Err) || !StartRun(R, Err))
			{
				R.Result = MakeShared<FJsonObject>();
				R.Result->SetStringField(TEXT("error"), TEXT("LAUNCH_FAILED"));
				R.Result->SetStringField(TEXT("message"), Err);
				Complete(R);
				return;
			}
		}
		const double Now = FPlatformTime::Seconds();
		if (Now - R.LastPoll < 0.25) { return; }
		R.LastPoll = Now;
		const bool bExited = !FPlatformProcess::IsProcRunning(R.Proc);
		ScanNewLines(R);
		const double Age = Now - R.T0;
		if (R.StopReason.IsEmpty() && !R.UrlOk.IsSet() && Age > R.S.UrlCheckTimeout) { R.StopReason = TEXT("URL_NOT_CONFIRMED"); }
		if (R.StopReason.IsEmpty() && R.RequireRe.IsSet() && !R.bRequireSeen && Age > R.S.RequireWithin) { R.StopReason = TEXT("REQUIRED_LINE_MISSING"); }
		if (R.StopReason.IsEmpty() && R.DoneAt >= 0 && !bExited && Now - R.DoneAt > R.S.DoneGrace) { R.StopReason = TEXT("DONE_BUT_NOT_EXITED"); }
		if (R.StopReason.IsEmpty() && R.S.ShotsExpected > 0 && !bExited && Now - R.LastShotCheck >= 1.0)
		{
			// Enough new PNG files, and their total size did not change since the last check (the last one is written).
			R.LastShotCheck = Now;
			const FIntPoint* Res = R.S.Resolutions.IsValidIndex(R.Index) ? &R.S.Resolutions[R.Index] : nullptr;
			int64 Bytes = 0;
			R.ShotsFound = CountNewPngs(R, Res, Bytes);
			if (R.ShotsFound >= R.S.ShotsExpected && Bytes > 0 && Bytes == R.ShotsBytes) { R.StopReason = TEXT("SHOTS_DONE"); }
			R.ShotsBytes = R.ShotsFound >= R.S.ShotsExpected ? Bytes : -1;
		}
		if (!(bExited || !R.StopReason.IsEmpty() || Age > R.S.Timeout)) { return; }
		int32 Code = -1;
		if (bExited) { FPlatformProcess::GetProcReturnCode(R.Proc, &Code); }
		FinishCurrent(R, bExited, Code);
		++R.Index;
		if (R.Index >= FMath::Max(1, R.S.Resolutions.Num())) { Complete(R); }
	}

	bool Tick(float)
	{
		const double Now = FPlatformTime::Seconds();
		bool bActive = false;
		for (auto& KV : Runs())
		{
			FGameRun& R = *KV.Value;
			if (R.bFinished) { continue; }
			Step(R);
			if (R.bFinished) { continue; }
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

	/** Number of Live Coding patches applied to this editor since it started (ER-07). */
	int32& LiveCodingPatches()
	{
		static int32 N = 0;
		return N;
	}

	void WatchLiveCoding()
	{
#if PLATFORM_WINDOWS
		static bool bHooked = false;
		if (bHooked) { return; }
		if (ILiveCodingModule* LC = FModuleManager::LoadModulePtr<ILiveCodingModule>(TEXT("LiveCoding")))
		{
			LC->GetOnPatchCompleteDelegate().AddLambda([]() { ++LiveCodingPatches(); });
			bHooked = true;
		}
#endif
	}

	/** Newest file time in Dir (recursive) for the given wildcards; files whose name contains Skip are ignored. */
	FDateTime Newest(const FString& Dir, const TArray<FString>& Patterns, const TCHAR* Skip, FString& Name)
	{
		FDateTime Best = FDateTime::MinValue();
		if (!IFileManager::Get().DirectoryExists(*Dir)) { return Best; }
		for (const FString& Pat : Patterns)
		{
			TArray<FString> Found;
			IFileManager::Get().FindFilesRecursive(Found, *Dir, *Pat, true, false);
			for (const FString& F : Found)
			{
				if (Skip && FPaths::GetCleanFilename(F).Contains(Skip)) { continue; }
				const FDateTime T = IFileManager::Get().GetTimeStamp(*F);
				if (T > Best) { Best = T; Name = Fwd(F); }
			}
		}
		return Best;
	}

	/**
	 * ER-07: a game started from this editor in -game mode loads the module DLLs on disk. Live Coding patches only the
	 * running editor, and C++ edited after the last build is not in any DLL. Returns false (refuse) when Live Coding
	 * patched the editor and the caller did not allow it; adds warnings and fills Checks either way.
	 */
	bool CheckCode(const FGameRun& R, bool bAllowPatched, TSharedRef<FJsonObject> Checks, TArray<FString>& Warnings, FString& RefuseMsg)
	{
		if (!R.S.Exe.IsEmpty()) { return true; } // a packaged exe has its own code
		const int32 Patches = LiveCodingPatches();
		Checks->SetNumberField(TEXT("liveCodingPatches"), Patches);
		if (Patches > 0)
		{
			const FString Msg = FString::Printf(TEXT("Live Coding patched this editor %d time(s) since it started. A new game process loads the module DLLs on disk, which do not contain those changes, so the game would run the old C++ code. Rebuild first: editor_restart {\"build\": true}."), Patches);
			if (!bAllowPatched) { RefuseMsg = Msg + TEXT(" To run the old code anyway, pass allowLiveCodingPatch: true."); return false; }
			Warnings.Add(Msg);
		}
		FString SrcFile, DllFile;
		const FDateTime Src = Newest(FPaths::GameSourceDir(), { TEXT("*.cpp"), TEXT("*.h"), TEXT("*.inl"), TEXT("*.Build.cs") }, nullptr, SrcFile);
		const FDateTime Dll = Newest(FPaths::ProjectDir() / TEXT("Binaries") / FPlatformProcess::GetBinariesSubdirectory(), { TEXT("UnrealEditor-*.dll") }, TEXT(".patch_"), DllFile);
		if (Src > FDateTime::MinValue() && Dll > FDateTime::MinValue())
		{
			const bool bStale = Src > Dll + FTimespan::FromSeconds(2);
			Checks->SetBoolField(TEXT("sourceNewerThanBuild"), bStale);
			if (bStale)
			{
				Warnings.Add(FString::Printf(TEXT("%s changed after the last build of the project's modules (%s UTC); the game runs the code that was built before. Rebuild first (editor_restart {\"build\": true}) if this change matters."),
					*FPaths::GetCleanFilename(SrcFile), *Dll.ToString(TEXT("%Y-%m-%d %H:%M:%S"))));
			}
		}
		return true;
	}

	/**
	 * ER-08: the editor keeps rendering its own level while the game runs. Reports how heavy the open level is and, unless
	 * told not to, pauses the realtime level viewports for the run (restored when it ends).
	 */
	void CheckEditorScene(FGameRun& R, bool bApply, TSharedRef<FJsonObject> Checks, TArray<FString>& Warnings)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) { return; }
		TSharedRef<FJsonObject> Scene = MakeShared<FJsonObject>();
		int32 Actors = 0;
		bool bCloud = false;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			++Actors;
			if (It->GetClass()->GetName().Contains(TEXT("VolumetricCloud"))) { bCloud = true; }
		}
		bool bRealtimeSky = false;
		for (TObjectIterator<USkyLightComponent> It; It; ++It)
		{
			if (It->GetWorld() == World && It->bRealTimeCapture && It->IsRegistered()) { bRealtimeSky = true; break; }
		}
		const IConsoleVariable* GI = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DynamicGlobalIlluminationMethod"));
		const bool bLumen = GI && GI->GetInt() == 1;
		int32 Realtime = 0;
		for (FLevelEditorViewportClient* V : GEditor->GetLevelViewportClients()) { if (V && V->IsRealtime()) { ++Realtime; } }
		Scene->SetStringField(TEXT("level"), World->GetOutermost()->GetName());
		Scene->SetNumberField(TEXT("actors"), Actors);
		Scene->SetBoolField(TEXT("lumen"), bLumen);
		Scene->SetBoolField(TEXT("realtimeSkyLight"), bRealtimeSky);
		Scene->SetBoolField(TEXT("volumetricClouds"), bCloud);
		Scene->SetNumberField(TEXT("realtimeViewports"), Realtime);
		const bool bHeavy = (bLumen && (bRealtimeSky || bCloud)) || Actors > 5000;
		Scene->SetBoolField(TEXT("heavy"), bHeavy);
		int32 Paused = 0;
		if (bApply)
		{
			for (FLevelEditorViewportClient* V : GEditor->GetLevelViewportClients())
			{
				if (V && V->IsRealtime()) { V->AddRealtimeOverride(false, PauseReason()); R.PausedViewports.Add(V); ++Paused; }
			}
		}
		Scene->SetNumberField(TEXT("viewportsPaused"), Paused);
		Checks->SetObjectField(TEXT("editorScene"), Scene);
		if (bHeavy)
		{
			Warnings.Add(FString::Printf(TEXT("The editor has a heavy level open (%s: %d actors%s%s%s); it shares the GPU with the game. %s If the game is still slow, open a small level in the editor first (level_open) and run again."),
				*World->GetOutermost()->GetName(), Actors, bLumen ? TEXT(", Lumen") : TEXT(""), bRealtimeSky ? TEXT(", real-time sky light") : TEXT(""), bCloud ? TEXT(", volumetric clouds") : TEXT(""),
				Paused > 0 ? TEXT("Its realtime viewports are paused during the run.") : (bApply ? TEXT("") : TEXT("Viewports were left running (editorViewports keep)."))));
		}
	}

	TOptional<FIntPoint> ParseRes(const FString& S)
	{
		FString L, Rr;
		FString T = S.TrimStartAndEnd().ToLower().Replace(TEXT("*"), TEXT("x"));
		if (!T.Split(TEXT("x"), &L, &Rr) || !L.IsNumeric() || !Rr.IsNumeric()) { return {}; }
		const int32 W = FCString::Atoi(*L), H = FCString::Atoi(*Rr);
		if (W < 16 || H < 16 || W > 16384 || H > 16384) { return {}; }
		return FIntPoint(W, H);
	}
}

void EWStopGameRuns()
{
	for (auto& KV : Runs())
	{
		FGameRun& R = *KV.Value;
		if (R.bProcStarted) { FPlatformProcess::TerminateProc(R.Proc, true); FPlatformProcess::CloseProc(R.Proc); R.bProcStarted = false; }
		RestoreViewports(R);
	}
	Runs().Empty();
	if (GTick.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(GTick); GTick.Reset(); }
}

void RegisterGameRunTools(FEWRegistry& Reg)
{
	WatchLiveCoding();
	Reg.RegisterAsync(TEXT("game_run"),
		TEXT("Start the game itself (UnrealEditor <project> <url> -game, or a packaged exe) once per resolution and report: did the URL arrive unchanged (checked in the game's own log), PASSED/FAILED from your patterns, the log lines you asked for, crash lines, and the new PNG files (screenshots) with their real size. ")
		TEXT("Use it for self-tests and screenshot runs at several resolutions. The editor stays usable while the game runs. The game loads the C++ modules built on disk, not the editor's memory: after C++ changes rebuild with editor_restart {\"build\": true}. Live Coding patches only the open editor, so game_run refuses to start after a Live Coding patch (allowLiveCodingPatch overrides). ")
		TEXT("While the game runs, the editor's realtime level viewports are paused (editorViewports) and a heavy editor level is reported. Screenshot runs (e.g. -ExecCmds=HighResShot) end with shotsExpected: the run is PASSED when that many new PNG files are written. ")
		TEXT("The game is a separate process: loc_culture changes only the editor's language and does not reach it. Pass culture (e.g. tr) to start the game with -culture=tr. Placeholders in url and args: {shots} (per-run folder for PNGs), {out}, {w}, {h}, {res}. If the result is not ready within 'wait' seconds the reply is STILL_RUNNING with an id; the game keeps running: follow it with game_run_status. Output: <project>/Saved/Enginewright/runs/run-<time>/ (summary.json, game.log and shots per resolution)."),
		TEXT(R"({"type":"object","properties":{
			"url":{"type":"string","description":"Map and options passed as one argument, unchanged, e.g. /Game/Maps/Test?SelfTest=Logic?Shots={shots}?Quit=1"},
			"resolutions":{"type":"array","description":"Resolutions as \"WxH\" strings, one run each in order (e.g. [\"1920x1080\",\"1280x720\"]). Default: one run at the game's own size."},
			"fullscreen":{"type":"boolean","description":"Fullscreen instead of windowed (default false)."},
			"args":{"type":"array","description":"Extra game arguments, e.g. [\"-nosound\",\"-ExecCmds=HighResShot 1\"]. Placeholders allowed."},
			"exe":{"type":"string","description":"A packaged game exe instead of this editor in -game mode."},
			"exeArgs":{"type":"array","description":"Arguments placed right after exe, before the URL."},
			"timeout":{"type":"number","description":"Seconds per run before the game is stopped (default 600)."},
			"filter":{"type":"array","description":"Keep log lines that contain any of these texts."},
			"pass":{"type":"array","description":"Regexes: a matching log line means PASSED."},
			"fail":{"type":"array","description":"Regexes: a matching log line means FAILED."},
			"done":{"type":"string","description":"Regex: the run is over when this line appears; the game is stopped doneGrace seconds later if it has not quit (stopReason DONE_BUT_NOT_EXITED, reported)."},
			"doneGrace":{"type":"number","description":"Seconds, default 20."},
			"require":{"type":"string","description":"Regex that must appear within requireWithin seconds, else the game is stopped (REQUIRED_LINE_MISSING). Guards against the real game running with real saves when the test mode did not start."},
			"requireWithin":{"type":"number","description":"Seconds, default 120."},
			"urlCheckTimeout":{"type":"number","description":"Seconds to wait for the game log's Command Line (default 120)."},
			"shots":{"type":"array","description":"Extra folders to collect new PNG files from (copied into the run folder). Saved/Screenshots is always searched for editor runs."},
			"out":{"type":"string","description":"Output folder (default <project>/Saved/Enginewright/runs/run-<time>)."},
			"maxLines":{"type":"integer","description":"Most kept log lines per run (default 300)."},
			"unattended":{"type":"boolean","description":"Pass -unattended (default true: no dialogs block the run)."},
			"wait":{"type":"number","description":"Seconds to wait for the result before replying STILL_RUNNING (default 300; 0 = reply at once with the id)."},
			"shotsExpected":{"type":"integer","description":"End the run when this many new PNG files exist (searched like the screenshots in the result) and are fully written; with no pass patterns the verdict is then PASSED (stopReason SHOTS_DONE). For screenshot runs whose game does not quit, e.g. -ExecCmds=HighResShot 1 with shotsExpected 1."},
			"culture":{"type":"string","description":"Culture the game starts with, e.g. tr or de-DE: adds -culture=<value> to the game arguments. loc_culture does not change the game process, only the editor."},
			"editorViewports":{"type":"string","enum":["pause","keep"],"description":"pause (default): the editor's realtime level viewports stop rendering during the run and are restored after; keep: leave them."},
			"allowLiveCodingPatch":{"type":"boolean","description":"Run even though Live Coding patched this editor (the game then runs the C++ code on disk, without the patch). Default false: refused with LIVE_CODING_PATCHED."},
			"dryRun":{"type":"boolean","description":"Return the commands and the editor checks only; start nothing, create no folder."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args, FEWDone Done)
		{
			for (const auto& KV : Runs())
			{
				if (!KV.Value->bFinished)
				{
					Done(FEWResult::Error(TEXT("BUSY"), FString::Printf(TEXT("Game run %s is still running; follow it with game_run_status, or stop it with game_run_status {\"id\":\"%s\",\"stop\":true}."), *KV.Key, *KV.Key)));
					return;
				}
			}
			TSharedPtr<FGameRun> R = MakeShared<FGameRun>();
			FRunSpec& S = R->S;
			S.Url = EW::Str(Args, TEXT("url"));
			S.Exe = EW::Str(Args, TEXT("exe"));
			S.ExeArgs = EW::StrList(Args, TEXT("exeArgs"));
			S.Args = EW::StrList(Args, TEXT("args"));
			S.Filters = EW::StrList(Args, TEXT("filter"));
			S.PassText = EW::StrList(Args, TEXT("pass"));
			S.FailText = EW::StrList(Args, TEXT("fail"));
			S.ShotDirs = EW::StrList(Args, TEXT("shots"));
			S.Done = EW::Str(Args, TEXT("done"));
			S.Require = EW::Str(Args, TEXT("require"));
			S.bFullscreen = EW::Bool(Args, TEXT("fullscreen"), false);
			S.bUnattended = EW::Bool(Args, TEXT("unattended"), true);
			S.Culture = EW::Str(Args, TEXT("culture")).TrimStartAndEnd();
			if (Args->HasField(TEXT("culture")))
			{
				if (S.Culture.IsEmpty()) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("culture is empty; pass a culture name such as tr or de-DE, or leave it out."))); return; }
				if (!FInternationalization::Get().GetCulture(S.Culture).IsValid())
				{
					TArray<FString> All;
					FInternationalization::Get().GetCultureNames(All);
					Done(FEWResult::Error(TEXT("UNKNOWN_CULTURE"), FString::Printf(TEXT("'%s' is not a known culture name."), *S.Culture), EW::Suggest(S.Culture, All)));
					return;
				}
				for (const FString& A : EW::StrList(Args, TEXT("args")))
				{
					if (A.StartsWith(TEXT("-culture="), ESearchCase::IgnoreCase)) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("args already contains -culture=; use only one of culture and that argument."))); return; }
				}
			}
			S.Timeout = EW::Num(Args, TEXT("timeout"), 600);
			S.DoneGrace = EW::Num(Args, TEXT("doneGrace"), 20);
			S.RequireWithin = EW::Num(Args, TEXT("requireWithin"), 120);
			S.UrlCheckTimeout = EW::Num(Args, TEXT("urlCheckTimeout"), 120);
			S.MaxLines = (int32)EW::Num(Args, TEXT("maxLines"), 300);
			S.ShotsExpected = (int32)EW::Num(Args, TEXT("shotsExpected"), 0);
			const FString Viewports = EW::Str(Args, TEXT("editorViewports"));
			if (!Viewports.IsEmpty() && Viewports != TEXT("pause") && Viewports != TEXT("keep"))
			{
				Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("editorViewports must be pause or keep, got '%s'."), *Viewports), { TEXT("pause"), TEXT("keep") }));
				return;
			}
			S.bPauseViewports = Viewports != TEXT("keep");
			const double Wait = EW::Num(Args, TEXT("wait"), 300);
			if (S.Timeout <= 0 || S.DoneGrace < 0 || S.RequireWithin <= 0 || S.UrlCheckTimeout <= 0 || Wait < 0 || S.MaxLines < 0 || S.ShotsExpected < 0 || S.ShotsExpected > 1000)
			{
				Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("timeout, requireWithin and urlCheckTimeout must be above 0; doneGrace, wait and maxLines 0 or more; shotsExpected 0 to 1000.")));
				return;
			}
			for (const FString& T : EW::StrList(Args, TEXT("resolutions")))
			{
				TOptional<FIntPoint> P = ParseRes(T);
				if (!P.IsSet()) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Resolution must look like 1920x1080 (16-16384), got '%s'."), *T))); return; }
				S.Resolutions.Add(P.GetValue());
			}
			if (!S.Exe.IsEmpty())
			{
				S.Exe = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), S.Exe);
				if (!IFileManager::Get().FileExists(*S.Exe)) { Done(FEWResult::Error(TEXT("NOT_FOUND"), FString::Printf(TEXT("exe not found: %s"), *S.Exe))); return; }
			}
			else if (!S.ExeArgs.IsEmpty()) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("exeArgs needs exe."))); return; }
			for (const FString& P : S.PassText) { R->Pass.Emplace(P); }
			for (const FString& P : S.FailText) { R->Fail.Emplace(P); }
			if (!S.Done.IsEmpty()) { R->DoneRe.Emplace(S.Done); }
			if (!S.Require.IsEmpty()) { R->RequireRe.Emplace(S.Require); }
			R->Id = FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
			const FString OutArg = EW::Str(Args, TEXT("out"));
			S.Out = OutArg.IsEmpty() ? FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Enginewright/runs") / (TEXT("run-") + R->Id))
				: FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), OutArg);
			FPaths::NormalizeDirectoryName(S.Out);

			// Check placeholders of every run before starting anything.
			TArray<TSharedPtr<FJsonValue>> Planned;
			for (R->Index = 0; R->Index < FMath::Max(1, S.Resolutions.Num()); ++R->Index)
			{
				FString Err;
				if (!PrepareRun(*R, Err)) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), Err)); return; }
				Planned.Add(MakeShared<FJsonValueObject>(R->Cur.ToSharedRef()));
			}
			R->Index = 0;
			const bool bDry = EW::Bool(Args, TEXT("dryRun"), false);
			TSharedRef<FJsonObject> Checks = MakeShared<FJsonObject>();
			FString Refuse;
			if (!CheckCode(*R, EW::Bool(Args, TEXT("allowLiveCodingPatch"), false), Checks, R->Warnings, Refuse) && !bDry)
			{
				FEWResult E = FEWResult::Error(TEXT("LIVE_CODING_PATCHED"), Refuse);
				E.Data = Checks;
				Done(E);
				return;
			}
			if (!Refuse.IsEmpty()) { R->Warnings.Add(Refuse); }
			CheckEditorScene(*R, S.bPauseViewports && !bDry, Checks, R->Warnings);
			R->Checks = Checks;
			if (bDry)
			{
				TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
				D->SetStringField(TEXT("out"), Fwd(S.Out));
				D->SetArrayField(TEXT("runs"), Planned);
				D->SetObjectField(TEXT("checks"), Checks);
				FEWResult Ok = FEWResult::Ok(D, TEXT("Dry run: nothing was started."));
				Ok.Warnings = R->Warnings;
				Done(Ok);
				return;
			}
			R->Started = FPlatformTime::Seconds();
			R->Waiters.Add({ R->Started + Wait, MoveTemp(Done) });
			Runs().Add(R->Id, R);
			EnsureTicker();
			Step(*R); // start the first game now
			if (R->bFinished) { return; } // launch failure already answered the waiter
			if (Wait <= 0)
			{
				FEWDone D = MoveTemp(R->Waiters.Last().Done);
				R->Waiters.Pop();
				D(RunningResult(*R));
			}
		});

	Reg.RegisterAsync(TEXT("game_run_status"),
		TEXT("Result of a game_run: waits up to 'wait' seconds for it to finish, then gives the same report as game_run (or STILL_RUNNING). stop true ends the running game now (its report says stopped). Without id: the latest run."),
		TEXT(R"({"type":"object","properties":{
			"id":{"type":"string","description":"Run id from game_run (default: the latest)."},
			"wait":{"type":"number","description":"Seconds to wait for the result (default 0)."},
			"stop":{"type":"boolean","description":"Stop the running game now."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args, FEWDone Done)
		{
			FString Id = EW::Str(Args, TEXT("id"));
			if (Id.IsEmpty())
			{
				TArray<FString> Keys;
				Runs().GetKeys(Keys);
				Keys.Sort();
				if (Keys.Num() == 0) { Done(FEWResult::Error(TEXT("NO_RUN"), TEXT("No game_run in this editor session."))); return; }
				Id = Keys.Last();
			}
			TSharedPtr<FGameRun>* Found = Runs().Find(Id);
			if (!Found)
			{
				TArray<FString> Keys;
				Runs().GetKeys(Keys);
				Done(FEWResult::Error(TEXT("NO_RUN"), FString::Printf(TEXT("No game run with id '%s' in this editor session."), *Id), EW::Suggest(Id, Keys)));
				return;
			}
			FGameRun& R = **Found;
			if (!R.bFinished && EW::Bool(Args, TEXT("stop"), false))
			{
				if (R.bProcStarted) { R.StopReason = TEXT("STOPPED"); FinishCurrent(R, false, -1); }
				R.Index = FMath::Max(1, R.S.Resolutions.Num()); // skip the remaining resolutions
				Complete(R);
			}
			if (R.bFinished) { Done(FinalResult(R)); return; }
			const double Wait = EW::Num(Args, TEXT("wait"), 0);
			if (Wait <= 0) { Done(RunningResult(R)); return; }
			R.Waiters.Add({ FPlatformTime::Seconds() + Wait, MoveTemp(Done) });
			EnsureTicker();
		});
}
