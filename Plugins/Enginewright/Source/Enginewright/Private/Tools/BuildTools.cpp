#include "EnginewrightTool.h"
#include "EWCommon.h"

#include "Editor.h"
#include "Editor/EditorPerformanceSettings.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#if PLATFORM_WINDOWS
#include "ILiveCodingModule.h"
#endif

namespace
{
	FString BuildDir() { return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Enginewright")); }
	FString BuildLog() { return BuildDir() / TEXT("build.log"); }

	TArray<FString> DirtyPackageNames()
	{
		TArray<UPackage*> Dirty;
		FEditorFileUtils::GetDirtyContentPackages(Dirty);
		FEditorFileUtils::GetDirtyWorldPackages(Dirty);
		TArray<FString> Out;
		for (UPackage* P : Dirty) { if (P) { Out.Add(P->GetName()); } }
		return Out;
	}

	/**
	 * Automation commands wait for an interactive frame rate (>= 10 FPS). An editor in the background is throttled
	 * below that, so an agent driving a background editor would time out. Keep the editor at full speed while
	 * automation tools wait; the setting is restored afterwards and never saved.
	 */
	int32 GNoThrottle = 0;
	bool GOldThrottle = true;
	void NoThrottleBegin()
	{
		UEditorPerformanceSettings* S = GetMutableDefault<UEditorPerformanceSettings>();
		if (GNoThrottle++ == 0) { GOldThrottle = S->bThrottleCPUWhenNotForeground; S->bThrottleCPUWhenNotForeground = false; }
	}
	void NoThrottleEnd()
	{
		if (GNoThrottle > 0 && --GNoThrottle == 0) { GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground = GOldThrottle; }
	}
	FEWDone Unthrottled(FEWDone Done)
	{
		NoThrottleBegin();
		return [Done](FEWResult R) { NoThrottleEnd(); Done(MoveTemp(R)); };
	}

	/** Parses the build log written by the restart script. */
	TSharedRef<FJsonObject> ReadBuildLog()
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		FString Text;
		J->SetStringField(TEXT("log"), BuildLog());
		if (!FFileHelper::LoadFileToString(Text, *BuildLog()))
		{
			J->SetStringField(TEXT("state"), TEXT("none"));
			return J;
		}
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines);
		FString State = TEXT("running"), ExitCode;
		TArray<TSharedPtr<FJsonValue>> Errors, Warnings;
		for (const FString& L : Lines)
		{
			if (L.StartsWith(TEXT("EW_EXIT="))) { ExitCode = L.Mid(8).TrimStartAndEnd(); State = ExitCode == TEXT("0") ? TEXT("succeeded") : TEXT("failed"); }
			else if (L.StartsWith(TEXT("EW_SKIPPED"))) { State = TEXT("skipped"); }
			else if ((L.Contains(TEXT(": error ")) || L.Contains(TEXT("error C")) || L.Contains(TEXT("error LNK")) || L.Contains(TEXT(": Error:"))) && Errors.Num() < 40) { Errors.Add(MakeShared<FJsonValueString>(L.TrimStartAndEnd())); }
			else if (L.Contains(TEXT(": warning ")) && !L.Contains(TEXT("Program Files")) && Warnings.Num() < 40) { Warnings.Add(MakeShared<FJsonValueString>(L.TrimStartAndEnd())); }
		}
		J->SetStringField(TEXT("state"), State);
		if (!ExitCode.IsEmpty()) { J->SetStringField(TEXT("exitCode"), ExitCode); }
		J->SetArrayField(TEXT("errors"), Errors);
		J->SetArrayField(TEXT("warnings"), Warnings);
		J->SetStringField(TEXT("written"), IFileManager::Get().GetTimeStamp(*BuildLog()).ToIso8601());
		TArray<TSharedPtr<FJsonValue>> Tail;
		for (int32 i = FMath::Max(0, Lines.Num() - 8); i < Lines.Num(); ++i) { Tail.Add(MakeShared<FJsonValueString>(Lines[i])); }
		J->SetArrayField(TEXT("tail"), Tail);
		return J;
	}
}

void RegisterBuildTools(FEWRegistry& R)
{
	R.Register(TEXT("editor_restart"),
		TEXT("Close the editor and open it again on this project, optionally building the project's C++ code in between (needed after header or Build.cs changes; Live Coding cannot do those). Refuses with unsaved changes unless save true. The reply comes before the editor closes; then poll editor_state until the server answers again and read build_status."),
		TEXT(R"({"type":"object","properties":{
			"build":{"type":"boolean","description":"Build the editor target (UnrealBuildTool) while the editor is closed. Default true."},
			"save":{"type":"boolean","description":"Save all unsaved packages first (untitled maps cannot be saved and block)."},
			"discardUnsaved":{"type":"boolean","description":"Close even with unsaved changes (they are lost)."},
			"reopen":{"type":"boolean","description":"Open the editor again after the build (default true; when the build fails the editor still opens with the old code)."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			if (EW::IsPlaying()) { return FEWResult::Error(TEXT("PIE_RUNNING"), TEXT("Stop Play In Editor first (pie_stop).")); }
			const bool bBuild = EW::Bool(Args, TEXT("build"), true);
			const bool bReopen = EW::Bool(Args, TEXT("reopen"), true);
			if (EW::Bool(Args, TEXT("save"), false))
			{
				FEditorFileUtils::SaveDirtyPackages(/*bPromptUser*/ false, /*bSaveMapPackages*/ true, /*bSaveContentPackages*/ true);
			}
			const TArray<FString> Dirty = DirtyPackageNames();
			if (Dirty.Num() && !EW::Bool(Args, TEXT("discardUnsaved"), false))
			{
				FEWResult Res = FEWResult::Error(TEXT("UNSAVED_CHANGES"), FString::Printf(TEXT("%d package(s) have unsaved changes: %s. Pass save true, or discardUnsaved true."), Dirty.Num(), *FString::Join(Dirty, TEXT(", ")).Left(400)));
				return Res;
			}
			const FString Project = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
			const FString Editor = FPaths::ConvertRelativePathToFull(FPlatformProcess::ExecutablePath());
			const FString EngineDir = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
			const FString Target = FString(FApp::GetProjectName()) + TEXT("Editor");
			const uint32 Pid = FPlatformProcess::GetCurrentProcessId();
			const bool bHasCode = IFileManager::Get().DirectoryExists(*(FPaths::ProjectDir() / TEXT("Source")));
#if PLATFORM_WINDOWS
			FString Bat = TEXT("@echo off\r\n");
			Bat += FString::Printf(TEXT("echo Enginewright restart %s > \"%s\"\r\n"), *FDateTime::Now().ToString(), *BuildLog());
			Bat += FString::Printf(TEXT(":wait\r\ntasklist /FI \"PID eq %u\" 2>nul | find \"%u\" >nul && (ping -n 2 127.0.0.1 >nul & goto wait)\r\n"), Pid, Pid);
			if (bBuild && bHasCode)
			{
				Bat += FString::Printf(TEXT("call \"%sBuild\\BatchFiles\\Build.bat\" %s Win64 Development -Project=\"%s\" -WaitMutex -NoHotReloadFromIDE >> \"%s\" 2>&1\r\n"),
					*EngineDir.Replace(TEXT("/"), TEXT("\\")), *Target, *Project.Replace(TEXT("/"), TEXT("\\")), *BuildLog());
				Bat += FString::Printf(TEXT("echo EW_EXIT=%%ERRORLEVEL%% >> \"%s\"\r\n"), *BuildLog());
			}
			else
			{
				Bat += FString::Printf(TEXT("echo EW_SKIPPED no build >> \"%s\"\r\n"), *BuildLog());
			}
			if (bReopen) { Bat += FString::Printf(TEXT("start \"\" \"%s\" \"%s\"\r\n"), *Editor.Replace(TEXT("/"), TEXT("\\")), *Project.Replace(TEXT("/"), TEXT("\\"))); }
			const FString BatFile = BuildDir() / TEXT("restart.bat");
			if (!FFileHelper::SaveStringToFile(Bat, *BatFile)) { return FEWResult::Error(TEXT("WRITE_FAILED"), TEXT("Could not write the restart script.")); }
			FProcHandle H = FPlatformProcess::CreateProc(TEXT("cmd.exe"), *FString::Printf(TEXT("/c \"%s\""), *BatFile.Replace(TEXT("/"), TEXT("\\"))),
				/*bLaunchDetached*/ true, /*bLaunchHidden*/ true, /*bLaunchReallyHidden*/ true, nullptr, 0, nullptr, nullptr);
			if (!H.IsValid()) { return FEWResult::Error(TEXT("LAUNCH_FAILED"), TEXT("Could not start the restart script.")); }
#else
			return FEWResult::Error(TEXT("NOT_SUPPORTED"), TEXT("editor_restart is Windows-only for now."));
#endif
			// Quit after the reply has been sent.
			EW::PollUntil(1.0f, []() { return false; }, []() { FPlatformMisc::RequestExit(false, TEXT("Enginewright editor_restart")); });
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("build"), bBuild && bHasCode);
			Data->SetBoolField(TEXT("reopen"), bReopen);
			Data->SetStringField(TEXT("log"), BuildLog());
			if (bBuild && !bHasCode) { Data->SetStringField(TEXT("note"), TEXT("The project has no Source folder; nothing to build.")); }
			return FEWResult::Ok(Data, TEXT("The editor closes now. Poll editor_state until it answers again, then call build_status."));
		});

	R.Register(TEXT("build_status"),
		TEXT("Result of the last editor_restart build: state (running, succeeded, failed, skipped), compiler errors and warnings, and the log tail."),
		TEXT(R"({"type":"object","properties":{}})"),
		false,
		[](const TSharedPtr<FJsonObject>&) -> FEWResult
		{
			TSharedRef<FJsonObject> Data = ReadBuildLog();
			FString State;
			Data->TryGetStringField(TEXT("state"), State);
			if (State == TEXT("failed"))
			{
				FEWResult Res = FEWResult::Error(TEXT("BUILD_FAILED"), TEXT("The last build failed; see data.errors. The editor runs the previous code."));
				Res.Data = Data;
				return Res;
			}
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("live_coding_compile"),
		TEXT("Compile changed C++ .cpp files with Live Coding and patch the running editor (Windows). Header, constructor default and Build.cs changes need editor_restart instead. Waits for the result."),
		TEXT(R"({"type":"object","properties":{}})"),
		false,
		[](const TSharedPtr<FJsonObject>&) -> FEWResult
		{
#if PLATFORM_WINDOWS
			if (EW::IsPlaying()) { return FEWResult::Error(TEXT("PIE_RUNNING"), TEXT("Stop Play In Editor first (pie_stop).")); }
			ILiveCodingModule* LC = FModuleManager::LoadModulePtr<ILiveCodingModule>(TEXT("LiveCoding"));
			if (!LC) { return FEWResult::Error(TEXT("NOT_AVAILABLE"), TEXT("Live Coding is not available in this editor.")); }
			if (!LC->IsEnabledForSession())
			{
				if (!LC->CanEnableForSession()) { return FEWResult::Error(TEXT("NOT_AVAILABLE"), FString::Printf(TEXT("Live Coding cannot start: %s"), *LC->GetEnableErrorText().ToString())); }
				LC->EnableForSession(true);
			}
			if (LC->IsCompiling()) { return FEWResult::Error(TEXT("BUSY"), TEXT("A Live Coding compile is already running.")); }
			const uint64 Seq = EW::LogSeq();
			ELiveCodingCompileResult Result = ELiveCodingCompileResult::NotStarted;
			LC->Compile(ELiveCodingCompileFlags::WaitForCompletion, &Result);
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			const TCHAR* Names[] = { TEXT("success"), TEXT("noChanges"), TEXT("inProgress"), TEXT("compileStillActive"), TEXT("notStarted"), TEXT("failure"), TEXT("cancelled") };
			Data->SetStringField(TEXT("result"), Names[(int32)Result]);
			TArray<TSharedPtr<FJsonValue>> Lines;
			for (const FString& L : EW::LogLinesSince(Seq, TEXT("LogLiveCoding"))) { if (Lines.Num() < 60) { Lines.Add(MakeShared<FJsonValueString>(L)); } }
			Data->SetArrayField(TEXT("log"), Lines);
			if (Result == ELiveCodingCompileResult::Success || Result == ELiveCodingCompileResult::NoChanges) { return FEWResult::Ok(Data); }
			FEWResult Res = FEWResult::Error(TEXT("LIVE_CODING_FAILED"), TEXT("Live Coding did not succeed; see data.log."));
			Res.Data = Data;
			return Res;
#else
			return FEWResult::Error(TEXT("NOT_SUPPORTED"), TEXT("Live Coding is Windows-only."));
#endif
		});

	R.RegisterAsync(TEXT("automation_run"),
		TEXT("Run automation tests whose names start with filter (e.g. Project.MyGame or System.Core.Math) in the editor and return each test's result. Use automation_list to see names."),
		TEXT(R"({"type":"object","properties":{
			"filter":{"type":"string"},
			"timeout":{"type":"number","description":"Seconds, default 300."}},
			"required":["filter"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args, FEWDone Done)
		{
			if (EW::IsPlaying()) { Done(FEWResult::Error(TEXT("PIE_RUNNING"), TEXT("Stop Play In Editor first (pie_stop)."))); return; }
			const FString Filter = EW::Str(Args, TEXT("filter")).TrimStartAndEnd();
			if (Filter.IsEmpty() || Filter.Contains(TEXT(";")) || Filter.Contains(TEXT("|"))) { Done(FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("filter must be one test name prefix."))); return; }
			Done = Unthrottled(MoveTemp(Done));
			const uint64 Seq = EW::LogSeq();
			GEditor->Exec(GEditor->GetEditorWorldContext().World(), *FString::Printf(TEXT("Automation RunTests %s"), *Filter));
			const float Timeout = (float)EW::Num(Args, TEXT("timeout"), 300);
			auto Collect = [Seq]()
			{
				TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> Tests;
				int32 Passed = 0, Failed = 0;
				for (const FString& L : EW::LogLinesSince(Seq, FString(), TEXT("Test Completed. Result={")))
				{
					FString Result, Name;
					L.Split(TEXT("Result={"), nullptr, &Result);
					Result.Split(TEXT("}"), &Result, nullptr);
					if (L.Split(TEXT("Path={"), nullptr, &Name) || L.Split(TEXT("Name={"), nullptr, &Name)) { Name.Split(TEXT("}"), &Name, nullptr); }
					TSharedRef<FJsonObject> T = MakeShared<FJsonObject>();
					T->SetStringField(TEXT("name"), Name);
					T->SetStringField(TEXT("result"), Result);
					Tests.Add(MakeShared<FJsonValueObject>(T));
					(Result == TEXT("Success") || Result == TEXT("Passed")) ? ++Passed : ++Failed;
				}
				TArray<TSharedPtr<FJsonValue>> Errors;
				for (const FString& L : EW::LogLinesSince(Seq, TEXT("LogAutomationController")))
				{
					if ((L.Contains(TEXT("Error")) || L.Contains(TEXT("error"))) && Errors.Num() < 30) { Errors.Add(MakeShared<FJsonValueString>(L)); }
				}
				Data->SetArrayField(TEXT("tests"), Tests);
				Data->SetNumberField(TEXT("passed"), Passed);
				Data->SetNumberField(TEXT("failed"), Failed);
				Data->SetArrayField(TEXT("errors"), Errors);
				return Data;
			};
			EW::PollUntil(Timeout,
				[Done, Seq, Collect, Filter]() -> bool
				{
					if (EW::LogLinesSince(Seq, FString(), TEXT("Automation Test Queue Empty")).Num() == 0)
					{
						if (EW::LogLinesSince(Seq, FString(), TEXT("No automation tests matched")).Num() == 0) { return false; }
						Done(FEWResult::Error(TEXT("NO_TESTS"), FString::Printf(TEXT("No automation test matches '%s' (automation_list)."), *Filter)));
						return true;
					}
					TSharedRef<FJsonObject> Data = Collect();
					const double Failed = Data->GetNumberField(TEXT("failed"));
					const double Passed = Data->GetNumberField(TEXT("passed"));
					if (Failed + Passed == 0) { Done(FEWResult::Error(TEXT("NO_TESTS"), FString::Printf(TEXT("No automation test ran for '%s' (automation_list)."), *Filter))); return true; }
					FEWResult Res = Failed > 0 ? FEWResult::Error(TEXT("TESTS_FAILED"), FString::Printf(TEXT("%d of %d test(s) failed."), (int32)Failed, (int32)(Failed + Passed))) : FEWResult::Ok();
					Res.Data = Data;
					Done(Res);
					return true;
				},
				[Done, Collect]()
				{
					FEWResult Res = FEWResult::Error(TEXT("TIMEOUT"), TEXT("The tests did not finish in time; partial results in data."));
					Res.Data = Collect();
					Done(Res);
				});
		});

	R.RegisterAsync(TEXT("automation_list"),
		TEXT("List automation test names that contain filter (e.g. Project., System.Core.Math)."),
		TEXT(R"({"type":"object","properties":{"filter":{"type":"string"},"limit":{"type":"integer","description":"Default 200."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args, FEWDone Done)
		{
			Done = Unthrottled(MoveTemp(Done));
			const uint64 Seq = EW::LogSeq();
			GEditor->Exec(GEditor->GetEditorWorldContext().World(), TEXT("Automation List"));
			const FString Filter = EW::Str(Args, TEXT("filter"));
			const int32 Limit = (int32)EW::Num(Args, TEXT("limit"), 200);
			EW::PollUntil(20.f,
				[Done, Seq, Filter, Limit]() -> bool
				{
					const TArray<FString> Lines = EW::LogLinesSince(Seq, TEXT("LogAutomationCommandLine"));
					bool bEnd = false;
					for (const FString& L : Lines) { if (L.Contains(TEXT("Automation Test Queue Empty"))) { bEnd = true; } }
					if (!bEnd) { return false; }
					TArray<TSharedPtr<FJsonValue>> Names;
					int32 Total = 0;
					for (const FString& L : Lines)
					{
						// Lines look like: "LogAutomationCommandLine: 	'System.Core.Math.Evaluate.Valid Expressions'"
						int32 Q = L.Find(TEXT("	'"));
						if (Q == INDEX_NONE) { continue; }
						FString Name = L.Mid(Q + 2);
						Name.RemoveFromEnd(TEXT("'"));
						Name.TrimStartAndEndInline();
						if (Name.IsEmpty()) { continue; }
						if (!Filter.IsEmpty() && !Name.Contains(Filter)) { continue; }
						++Total;
						if (Names.Num() < Limit) { Names.Add(MakeShared<FJsonValueString>(Name)); }
					}
					TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("tests"), Names);
					Data->SetNumberField(TEXT("matched"), Total);
					Done(FEWResult::Ok(Data));
					return true;
				},
				[Done]() { Done(FEWResult::Error(TEXT("TIMEOUT"), TEXT("The automation test list did not arrive in time."))); });
		});
}
