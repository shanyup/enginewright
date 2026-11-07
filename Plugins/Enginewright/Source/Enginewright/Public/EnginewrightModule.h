#pragma once

#include "Modules/ModuleInterface.h"
#include "HttpRouteHandle.h"
#include "HttpResultCallback.h"
#include "Dom/JsonObject.h"

class IHttpRouter;
struct FHttpServerRequest;

class FEnginewrightModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Server URL and token, for the client config tool. */
	FString GetUrl() const;
	const FString& GetToken() const { return Token; }

private:
	bool HandlePost(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	/** Calls Reply with the JSON-RPC reply, or an empty string for a notification. */
	void HandleRpc(const TSharedPtr<FJsonObject>& Msg, TFunction<void(const FString&)> Reply);
	TSharedRef<FJsonObject> ToolsList() const;
	void CallTool(const FString& Name, const TSharedPtr<FJsonObject>& Args, TFunction<void(TSharedRef<FJsonObject>)> Done) const;

	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> Routes;
	FString Token;
	uint32 Port = 3100;
};
