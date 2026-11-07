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

private:
	bool HandlePost(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	/** Returns the JSON-RPC reply, or an empty string for a notification. */
	FString HandleRpc(const TSharedPtr<FJsonObject>& Msg);
	TSharedRef<FJsonObject> ToolsList() const;
	TSharedRef<FJsonObject> CallTool(const FString& Name, const TSharedPtr<FJsonObject>& Args) const;

	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> Routes;
	FString Token;
	uint32 Port = 3100;
};
