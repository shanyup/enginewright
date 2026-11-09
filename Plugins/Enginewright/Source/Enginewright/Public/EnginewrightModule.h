#pragma once

#include "Modules/ModuleInterface.h"
#include "HttpRouteHandle.h"
#include "HttpResultCallback.h"
#include "Dom/JsonObject.h"

class IHttpRouter;
struct FHttpServerRequest;

/** A client that talked to this editor (one MCP session). */
struct FEWClientSeen
{
	FString Session;
	FString Name;
	FString Version;
	FDateTime FirstSeen;
	FDateTime LastSeen;
	int32 Requests = 0;
};

class FEnginewrightModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Server URL and token, for the client config tool. */
	FString GetUrl() const;
	const FString& GetToken() const { return Token; }

	/** True while the /mcp route is bound and the port listens. */
	bool IsListening() const { return bListening; }
	uint32 GetPort() const { return Port; }
	/** Why the server is not listening (empty when it is, or when it was turned off in the settings). */
	const FString& GetServerError() const { return ServerError; }
	/** True when ENGINEWRIGHT_PORT overrides the port in the settings. */
	bool IsPortFromEnvironment() const { return bPortFromEnv; }

	/** New random token, written to Saved/Enginewright/token and the client configs. Clients must be set up again. */
	void RegenerateToken();
	/** Clients seen within the last WithinSeconds, newest first. */
	TArray<FEWClientSeen> GetClients(double WithinSeconds = 300.0) const;

	static FEnginewrightModule& Get();

private:
	bool HandlePost(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	/** Calls Reply with the JSON-RPC reply, or an empty string for a notification. */
	void HandleRpc(const TSharedPtr<FJsonObject>& Msg, TFunction<void(const FString&)> Reply);
	TSharedRef<FJsonObject> ToolsList() const;
	void CallTool(const FString& Name, const TSharedPtr<FJsonObject>& Args, TFunction<void(TSharedRef<FJsonObject>)> Done) const;

	void StartServer();
	void StopServer();
	/** Starts, stops or moves the server after a settings change. */
	void ApplySettings();
	uint32 WantedPort();

	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> Routes;
	FString Token;
	uint32 Port = 3100;
	bool bListening = false;
	bool bPortFromEnv = false;
	FString ServerError;
	TMap<FString, FEWClientSeen> Clients;
	FDelegateHandle SettingsChangedHandle;
};
