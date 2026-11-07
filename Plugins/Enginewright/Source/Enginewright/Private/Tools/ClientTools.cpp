#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EnginewrightModule.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

namespace
{
	struct FClientConfig
	{
		const TCHAR* Client;
		const TCHAR* File;
		const TCHAR* Where;
		FString Text;
	};

	TArray<FClientConfig> BuildConfigs(const FString& Url, const FString& Token)
	{
		TArray<FClientConfig> Out;
		Out.Add({ TEXT("claude-code"), TEXT("claude-code.mcp.json"),
			TEXT("Merge into .mcp.json at your project root, or run the command in claude-code.txt."),
			FString::Printf(TEXT("{\n  \"mcpServers\": {\n    \"enginewright\": {\n      \"type\": \"http\",\n      \"url\": \"%s\",\n      \"headers\": { \"X-Enginewright-Token\": \"%s\" }\n    }\n  }\n}\n"), *Url, *Token) });
		Out.Add({ TEXT("claude-code-cli"), TEXT("claude-code.txt"),
			TEXT("Run once in a terminal in your project folder."),
			FString::Printf(TEXT("claude mcp add --transport http enginewright %s --header \"X-Enginewright-Token: %s\"\n"), *Url, *Token) });
		Out.Add({ TEXT("codex"), TEXT("codex.toml"),
			TEXT("Append to ~/.codex/config.toml."),
			FString::Printf(TEXT("[mcp_servers.enginewright]\nurl = \"%s\"\nhttp_headers = { \"X-Enginewright-Token\" = \"%s\" }\n"), *Url, *Token) });
		Out.Add({ TEXT("cursor"), TEXT("cursor.mcp.json"),
			TEXT("Merge into .cursor/mcp.json in your project (or ~/.cursor/mcp.json)."),
			FString::Printf(TEXT("{\n  \"mcpServers\": {\n    \"enginewright\": {\n      \"url\": \"%s\",\n      \"headers\": { \"X-Enginewright-Token\": \"%s\" }\n    }\n  }\n}\n"), *Url, *Token) });
		Out.Add({ TEXT("vscode"), TEXT("vscode.mcp.json"),
			TEXT("Merge into .vscode/mcp.json in your workspace."),
			FString::Printf(TEXT("{\n  \"servers\": {\n    \"enginewright\": {\n      \"type\": \"http\",\n      \"url\": \"%s\",\n      \"headers\": { \"X-Enginewright-Token\": \"%s\" }\n    }\n  }\n}\n"), *Url, *Token) });
		Out.Add({ TEXT("hermes"), TEXT("hermes.yaml"),
			TEXT("Merge under mcp_servers in your Hermes config.yaml."),
			FString::Printf(TEXT("mcp_servers:\n  enginewright:\n    url: %s\n    headers:\n      X-Enginewright-Token: %s\n    connect_timeout: 10\n"), *Url, *Token) });
		return Out;
	}
}

namespace EW
{
	void WriteClientConfigs(const FString& Url, const FString& Token, const FString& Dir)
	{
		const TArray<FClientConfig> Configs = BuildConfigs(Url, Token);
		// Kept for older setups: the generic config is the Claude Code one.
		FFileHelper::SaveStringToFile(Configs[0].Text, *(Dir / TEXT("client-config.json")));
		FString Readme = TEXT("Enginewright client configs. Each file holds this project's token: do not commit them.\n\n");
		for (const FClientConfig& C : Configs)
		{
			FFileHelper::SaveStringToFile(C.Text, *(Dir / TEXT("clients") / C.File));
			Readme += FString::Printf(TEXT("%s: %s\n"), C.File, C.Where);
		}
		FFileHelper::SaveStringToFile(Readme, *(Dir / TEXT("clients") / TEXT("README.txt")));
	}
}

void RegisterClientTools(FEWRegistry& R)
{
	R.Register(TEXT("client_config"),
		TEXT("Get ready-to-paste MCP client configs for this editor (Claude Code, Codex, Cursor, VS Code, Hermes) with the server URL and token. The same files are in Saved/Enginewright/clients/."),
		TEXT(R"({"type":"object","properties":{
			"client":{"type":"string","enum":["claude-code","claude-code-cli","codex","cursor","vscode","hermes"],"description":"Only this client; omit for all."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const FEnginewrightModule& M = FModuleManager::GetModuleChecked<FEnginewrightModule>(TEXT("Enginewright"));
			const FString Want = EW::Str(Args, TEXT("client"));
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("url"), M.GetUrl());
			Data->SetStringField(TEXT("folder"), FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Enginewright/clients")));
			TArray<TSharedPtr<FJsonValue>> List;
			for (const FClientConfig& C : BuildConfigs(M.GetUrl(), M.GetToken()))
			{
				if (!Want.IsEmpty() && !Want.Equals(C.Client, ESearchCase::IgnoreCase)) { continue; }
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("client"), C.Client);
				J->SetStringField(TEXT("file"), C.File);
				J->SetStringField(TEXT("where"), C.Where);
				J->SetStringField(TEXT("text"), C.Text);
				List.Add(MakeShared<FJsonValueObject>(J));
			}
			Data->SetArrayField(TEXT("configs"), List);
			FEWResult Res = FEWResult::Ok(Data);
			Res.Warnings.Add(TEXT("The text holds this project's token. Do not commit it to a public repository."));
			return Res;
		});
}
