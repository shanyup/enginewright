#include "EnginewrightTool.h"
#include "EWCommon.h"

#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/EngineVersion.h"
#include "Misc/StringOutputDevice.h"
#include "PlayInEditorDataTypes.h"

void RegisterEditorTools(FEWRegistry& R)
{
	R.Register(TEXT("editor_state"),
		TEXT("Read the editor state: whether Play In Editor runs, the open map, and the server version."),
		TEXT(R"({"type":"object","properties":{}})"),
		false,
		[](const TSharedPtr<FJsonObject>&) -> FEWResult
		{
			TSharedRef<FJsonObject> Data = EW::EditorState();
			Data->SetStringField(TEXT("server"), TEXT("Enginewright 0.1.0-dev"));
			Data->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("console_command"),
		TEXT("Run a console command. While Play In Editor runs it goes through the first player (cheats work); otherwise the editor world. Returns the text the command printed."),
		TEXT(R"({"type":"object","properties":{
			"command":{"type":"string","description":"Console command, e.g. 'stat fps' or a cheat."}},
			"required":["command"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const FString Cmd = EW::Str(Args, TEXT("command"));
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("command"), Cmd);

			if (GEditor && GEditor->PlayWorld)
			{
				APlayerController* PC = GEditor->PlayWorld->GetFirstPlayerController();
				if (!PC) { return FEWResult::Error(TEXT("NO_PLAYER"), TEXT("Play In Editor runs but has no player controller yet.")); }
				Data->SetStringField(TEXT("target"), TEXT("pie"));
				Data->SetStringField(TEXT("output"), PC->ConsoleCommand(Cmd, true));
				return FEWResult::Ok(Data);
			}

			FStringOutputDevice Out;
			Out.SetAutoEmitLineTerminator(true);
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			const bool bHandled = GEditor && GEditor->Exec(World, *Cmd, Out);
			Data->SetStringField(TEXT("target"), TEXT("editor"));
			Data->SetStringField(TEXT("output"), Out);
			FEWResult Res = FEWResult::Ok(Data);
			if (!bHandled) { Res.Warnings.Add(TEXT("No handler reported this command as handled; check the spelling.")); }
			return Res;
		});

	R.Register(TEXT("pie_start"),
		TEXT("Start Play In Editor in the viewport. Returns at once; the session starts on the next frames (poll editor_state.playing)."),
		TEXT(R"({"type":"object","properties":{
			"simulate":{"type":"boolean","description":"Simulate instead of play (no player)."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			if (EW::IsPlaying()) { return FEWResult::Error(TEXT("PIE_RUNNING"), TEXT("Play In Editor already runs.")); }
			bool bSimulate = false;
			Args->TryGetBoolField(TEXT("simulate"), bSimulate);
			FRequestPlaySessionParams Params;
			Params.WorldType = bSimulate ? EPlaySessionWorldType::SimulateInEditor : EPlaySessionWorldType::PlayInEditor;
			GEditor->RequestPlaySession(Params);
			return FEWResult::Ok(nullptr, TEXT("Play In Editor requested."));
		});

	R.Register(TEXT("pie_stop"),
		TEXT("Stop Play In Editor."),
		TEXT(R"({"type":"object","properties":{}})"),
		false,
		[](const TSharedPtr<FJsonObject>&) -> FEWResult
		{
			if (!EW::IsPlaying()) { return FEWResult::Ok(nullptr, TEXT("Play In Editor was not running.")); }
			GEditor->RequestEndPlayMap();
			return FEWResult::Ok(nullptr, TEXT("Stop requested."));
		});
}
