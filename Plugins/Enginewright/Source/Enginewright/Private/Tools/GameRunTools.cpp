// game_run / game_run_status: start the game itself (UnrealEditor <project> <url> -game, or a packaged exe) once
// per resolution, check that the URL arrived unchanged, keep the log lines you ask for, decide PASSED/FAILED,
// and collect the PNG files it wrote (ER-02). Runs inside the editor without blocking it; no Python.
#include "EnginewrightTool.h"
#include "EWCommon.h"

#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Internationalization/Regex.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"

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
		FString Url, Exe, Out, Done, Require;
		TArray<FString> ExeArgs, Args, Filters, PassText, FailText, ShotDirs;
		TArray<FIntPoint> Resolutions; // empty = one run at the default size
		bool bFullscreen = false, bUnattended = true;
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
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> CollectPngs(const FGameRun& R, const FIntPoint* Res)
	{
		TArray<FString> Dirs = { R.ShotsDir };
		const TMap<FString, FString> Vals = PlaceholderValues(R, Res);
		for (const FString& D : R.S.ShotDirs)
		{
			FString Filled, Err;
			if (FillPlaceholders(D, Vals, Filled, Err)) { Dirs.Add(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Filled)); }
		}
		if (R.S.Exe.IsEmpty()) { Dirs.Add(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Screenshots"))); }
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
		// The last line may still be half written: scan it again next time.
		const int32 Complete = R.Text.EndsWith(TEXT("\n")) ? Lines.Num() : Lines.Num() - 1;
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

	void Complete(FGameRun& R)
	{
		R.bFinished = true;
		if (!R.Result.IsValid()) { R.Result = MakeShared<FJsonObject>(); }
		R.Result->SetStringField(TEXT("id"), R.Id);
		R.Result->SetStringField(TEXT("state"), TEXT("finished"));
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
	}
	Runs().Empty();
	if (GTick.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(GTick); GTick.Reset(); }
}

void RegisterGameRunTools(FEWRegistry& Reg)
{
	Reg.RegisterAsync(TEXT("game_run"),
		TEXT("Start the game itself (UnrealEditor <project> <url> -game, or a packaged exe) once per resolution and report: did the URL arrive unchanged (checked in the game's own log), PASSED/FAILED from your patterns, the log lines you asked for, crash lines, and the new PNG files (screenshots) with their real size. ")
		TEXT("Use it for self-tests and screenshot runs at several resolutions. The editor stays usable while the game runs. The game runs the code the editor has loaded: after C++ changes use live_coding_compile or editor_restart first. ")
		TEXT("Placeholders in url and args: {shots} (per-run folder for PNGs), {out}, {w}, {h}, {res}. If the result is not ready within 'wait' seconds the reply is STILL_RUNNING with an id; the game keeps running: follow it with game_run_status. Output: <project>/Saved/Enginewright/runs/run-<time>/ (summary.json, game.log and shots per resolution)."),
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
			"dryRun":{"type":"boolean","description":"Return the commands only; start nothing, create no folder."}}})"),
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
			S.Timeout = EW::Num(Args, TEXT("timeout"), 600);
			S.DoneGrace = EW::Num(Args, TEXT("doneGrace"), 20);
			S.RequireWithin = EW::Num(Args, TEXT("requireWithin"), 120);
			S.UrlCheckTimeout = EW::Num(Args, TEXT("urlCheckTimeout"), 120);
			S.MaxLines = (int32)EW::Num(Args, TEXT("maxLines"), 300);
			const double Wait = EW::Num(Args, TEXT("wait"), 300);
			if (S.Timeout <= 0 || S.DoneGrace < 0 || S.RequireWithin <= 0 || S.UrlCheckTimeout <= 0 || Wait < 0 || S.MaxLines < 0)
			{
				Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("timeout, requireWithin and urlCheckTimeout must be above 0; doneGrace, wait and maxLines 0 or more.")));
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
			if (EW::Bool(Args, TEXT("dryRun"), false))
			{
				TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
				D->SetStringField(TEXT("out"), Fwd(S.Out));
				D->SetArrayField(TEXT("runs"), Planned);
				Done(FEWResult::Ok(D, TEXT("Dry run: nothing was started.")));
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
