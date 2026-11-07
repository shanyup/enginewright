#include "EnginewrightTool.h"
#include "EWCommon.h"

#include "Editor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/PlayerController.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "Misc/StringOutputDevice.h"

void RegisterEditorTools(FEWRegistry& R)
{
	R.Register(TEXT("editor_state"),
		TEXT("Read the editor state: whether Play In Editor runs, the open map, the engine and server version."),
		TEXT(R"({"type":"object","properties":{}})"),
		false,
		[](const TSharedPtr<FJsonObject>&) -> FEWResult
		{
			TSharedRef<FJsonObject> Data = EW::EditorState();
			Data->SetStringField(TEXT("server"), FString(TEXT("Enginewright ")) + ENGINEWRIGHT_VERSION);
			Data->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
			Data->SetStringField(TEXT("project"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("console_command"),
		TEXT("Run a console command. While Play In Editor runs it goes through a player (cheats work; choose the player for multiplayer tests); otherwise the editor world. Returns the text the command printed."),
		TEXT(R"({"type":"object","properties":{
			"command":{"type":"string","description":"Console command, e.g. 'stat fps' or a cheat."},
			"player":{"type":"integer","description":"Play In Editor local player index (0 = first / host). Default 0."},
			"target":{"type":"string","enum":["auto","editor"],"description":"editor = run in the editor even while playing."}},
			"required":["command"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const FString Cmd = EW::Str(Args, TEXT("command"));
			if (Cmd.TrimStartAndEnd().IsEmpty()) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("command is empty.")); }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("command"), Cmd);

			if (EW::IsPlaying() && !EW::Str(Args, TEXT("target")).Equals(TEXT("editor"), ESearchCase::IgnoreCase))
			{
				FString Err;
				const int32 Player = (int32)EW::Num(Args, TEXT("player"), 0);
				APlayerController* PC = EW::PiePlayer(Player, Err);
				if (!PC) { return FEWResult::Error(TEXT("NO_PLAYER"), Err); }
				Data->SetStringField(TEXT("target"), TEXT("pie"));
				Data->SetNumberField(TEXT("player"), Player);
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

	R.Register(TEXT("level_open"),
		TEXT("Open a level (map) in the editor. Refuses when the current level or other packages have unsaved changes, unless discard is true."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string","description":"Map path, e.g. /Game/Maps/Arena."},
			"discard":{"type":"boolean","description":"Open even if the current level has unsaved changes (they are lost)."}},
			"required":["path"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Path = EW::Str(Args, TEXT("path"));
			int32 Dot;
			if (Path.FindChar(TEXT('.'), Dot)) { Path = Path.Left(Dot); }
			if (!FPackageName::DoesPackageExist(Path)) { return FEWResult::Error(TEXT("MAP_NOT_FOUND"), FString::Printf(TEXT("No map at '%s'."), *Path)); }
			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!EW::Bool(Args, TEXT("discard"), false) && World && World->GetOutermost()->IsDirty())
			{
				return FEWResult::Error(TEXT("UNSAVED_CHANGES"), TEXT("The open level has unsaved changes. Save it (level_save) or pass discard true."));
			}
			if (World) { World->GetOutermost()->SetDirtyFlag(false); }
			if (!FEditorFileUtils::LoadMap(Path, false, true)) { return FEWResult::Error(TEXT("OPEN_FAILED"), TEXT("The editor could not open the map.")); }
			const FString Now = GEditor->GetEditorWorldContext().World()->GetOutermost()->GetName();
			if (Now != Path) { return FEWResult::Error(TEXT("READBACK_MISMATCH"), FString::Printf(TEXT("The open map is %s."), *Now)); }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("map"), Now);
			return FEWResult::Ok(Data);
		}, /*bTransactional*/ false);

	R.Register(TEXT("level_save"),
		TEXT("Save the open level, or save it as a new map with path (the editor then works on the new map)."),
		TEXT(R"({"type":"object","properties":{"path":{"type":"string","description":"Save as this new map path; omit to save in place."}}})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World) { return FEWResult::Error(TEXT("NO_LEVEL"), TEXT("No level is open.")); }
			const FString Path = EW::Str(Args, TEXT("path"));
			bool bOk = false;
			if (Path.IsEmpty())
			{
				if (World->GetOutermost()->GetName().StartsWith(TEXT("/Engine/")) || World->GetOutermost()->GetName().StartsWith(TEXT("/Temp/")))
				{
					return FEWResult::Error(TEXT("READ_ONLY_MAP"), TEXT("This map is an engine or untitled map; pass path to save it as a new map under /Game."));
				}
				bOk = FEditorFileUtils::SaveLevel(World->PersistentLevel);
			}
			else
			{
				FString Folder, Name, Err;
				if (!EW::SplitAssetPath(Path, Folder, Name, Err)) { return FEWResult::Error(TEXT("INVALID_PATH"), Err); }
				if (FPackageName::DoesPackageExist(Folder / Name)) { return FEWResult::Error(TEXT("ASSET_EXISTS"), FString::Printf(TEXT("'%s' already exists."), *(Folder / Name))); }
				bOk = FEditorFileUtils::SaveMap(World, Folder / Name);
			}
			if (!bOk) { return FEWResult::Error(TEXT("SAVE_FAILED"), TEXT("The level could not be saved.")); }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("map"), GEditor->GetEditorWorldContext().World()->GetOutermost()->GetName());
			Data->SetBoolField(TEXT("dirty"), GEditor->GetEditorWorldContext().World()->GetOutermost()->IsDirty());
			return FEWResult::Ok(Data);
		}, /*bTransactional*/ false);
}
