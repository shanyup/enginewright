#include "EnginewrightModule.h"
#include "EnginewrightTool.h"
#include "EWCommon.h"

#include "HAL/PlatformMisc.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

void RegisterEditorTools(FEWRegistry& R);
void RegisterObjectTools(FEWRegistry& R);
void RegisterAssetTools(FEWRegistry& R);
void RegisterDataTableTools(FEWRegistry& R);
void RegisterSceneTools(FEWRegistry& R);
void RegisterSettingsLogTools(FEWRegistry& R);
void RegisterBatchTool(FEWRegistry& R);
void RegisterClientTools(FEWRegistry& R);
void RegisterTypeTools(FEWRegistry& R);
void RegisterBlueprintTools(FEWRegistry& R);
void RegisterPieTools(FEWRegistry& R);
void RegisterBuildTools(FEWRegistry& R);
void RegisterMaterialTools(FEWRegistry& R);
void RegisterMeshTextureTools(FEWRegistry& R);
void RegisterImportTools(FEWRegistry& R);
void RegisterInputTools(FEWRegistry& R);
void RegisterValidationTools(FEWRegistry& R);
void RegisterAuditTools(FEWRegistry& R);
void RegisterMultiplayerTools(FEWRegistry& R);
void RegisterMeasureTools(FEWRegistry& R);
void RegisterUMGTools(FEWRegistry& R);
void RegisterGameplayTagTools(FEWRegistry& R);
namespace EW
{
	void InstallLogCapture();
	void RemoveLogCapture();
	void WriteClientConfigs(const FString& Url, const FString& Token, const FString& Dir);
}

static const TCHAR* GInstructions =
	TEXT("Enginewright drives the running Unreal Editor. Use search with 2-4 plain words to find a tool, describe to read its arguments, call to run it. ")
	TEXT("Every reply has ok, data, warnings and the editor state. Unknown arguments, properties and struct fields are errors, never ignored. ")
	TEXT("Tools that change assets are blocked while Play In Editor runs. Paths start with /Game.");

namespace
{
	TSharedRef<FJsonObject> Schema(const TCHAR* Json)
	{
		TSharedPtr<FJsonObject> O = EW::ParseObject(Json);
		return O.IsValid() ? O.ToSharedRef() : MakeShared<FJsonObject>();
	}

	TSharedRef<FJsonObject> ToolEntry(const FString& Name, const FString& Desc, const TSharedRef<FJsonObject>& InputSchema)
	{
		TSharedRef<FJsonObject> T = MakeShared<FJsonObject>();
		T->SetStringField(TEXT("name"), Name);
		T->SetStringField(TEXT("description"), Desc);
		T->SetObjectField(TEXT("inputSchema"), InputSchema);
		return T;
	}

	FString FirstSentence(const FString& S)
	{
		int32 Dot = INDEX_NONE;
		return S.FindChar(TEXT('.'), Dot) ? S.Left(Dot + 1) : S;
	}

	FString RpcResult(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Result)
	{
		TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		M->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
		M->SetObjectField(TEXT("result"), Result);
		return EW::ToJsonString(M);
	}

	FString RpcError(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message)
	{
		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetNumberField(TEXT("code"), Code);
		E->SetStringField(TEXT("message"), Message);
		TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		M->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
		M->SetObjectField(TEXT("error"), E);
		return EW::ToJsonString(M);
	}

	TUniquePtr<FHttpServerResponse> Reply(EHttpServerResponseCodes Code, const FString& Body)
	{
		TUniquePtr<FHttpServerResponse> R = FHttpServerResponse::Create(Body, TEXT("application/json"));
		R->Code = Code;
		return R;
	}
}

FString FEnginewrightModule::GetUrl() const
{
	return FString::Printf(TEXT("http://127.0.0.1:%u/mcp"), Port);
}

void FEnginewrightModule::StartupModule()
{
	FEWRegistry& Reg = FEWRegistry::Get();
	EW::InstallLogCapture();
	RegisterEditorTools(Reg);
	RegisterObjectTools(Reg);
	RegisterAssetTools(Reg);
	RegisterDataTableTools(Reg);
	RegisterSceneTools(Reg);
	RegisterSettingsLogTools(Reg);
	RegisterBatchTool(Reg);
	RegisterClientTools(Reg);
	RegisterTypeTools(Reg);
	RegisterBlueprintTools(Reg);
	RegisterPieTools(Reg);
	RegisterBuildTools(Reg);
	RegisterMaterialTools(Reg);
	RegisterMeshTextureTools(Reg);
	RegisterImportTools(Reg);
	RegisterInputTools(Reg);
	RegisterValidationTools(Reg);
	RegisterAuditTools(Reg);
	RegisterMultiplayerTools(Reg);
	RegisterMeasureTools(Reg);
	RegisterUMGTools(Reg);
	RegisterGameplayTagTools(Reg);

	const FString EnvPort = FPlatformMisc::GetEnvironmentVariable(TEXT("ENGINEWRIGHT_PORT"));
	if (!EnvPort.IsEmpty()) { Port = FCString::Atoi(*EnvPort); }

	// Per-project token; clients send it as X-Enginewright-Token or Authorization: Bearer.
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("Enginewright");
	const FString TokenFile = Dir / TEXT("token");
	if (!FFileHelper::LoadFileToString(Token, *TokenFile) || Token.TrimStartAndEnd().IsEmpty())
	{
		Token = FGuid::NewGuid().ToString(EGuidFormats::Digits) + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		FFileHelper::SaveStringToFile(Token, *TokenFile);
	}
	Token = Token.TrimStartAndEnd();
	EW::WriteClientConfigs(GetUrl(), Token, Dir);

	Router = FHttpServerModule::Get().GetHttpRouter(Port, /*bFailOnBindFailure*/ true);
	if (!Router.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Enginewright: could not listen on port %u (set ENGINEWRIGHT_PORT to use another)."), Port);
		return;
	}

	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/mcp")), EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateRaw(this, &FEnginewrightModule::HandlePost)));
	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/mcp")), EHttpServerRequestVerbs::VERB_GET | EHttpServerRequestVerbs::VERB_DELETE,
		FHttpRequestHandler::CreateLambda([](const FHttpServerRequest&, const FHttpResultCallback& OnComplete)
		{
			// No server-sent stream: every reply comes back on the POST.
			OnComplete(Reply(EHttpServerResponseCodes::BadMethod, TEXT("{}")));
			return true;
		})));
	FHttpServerModule::Get().StartAllListeners();
	UE_LOG(LogTemp, Display, TEXT("Enginewright: MCP server on %s (%d tools). Client configs: %s"),
		*GetUrl(), Reg.All().Num(), *(Dir / TEXT("clients")));
}

void FEnginewrightModule::ShutdownModule()
{
	if (Router.IsValid())
	{
		for (const FHttpRouteHandle& H : Routes) { Router->UnbindRoute(H); }
	}
	Routes.Empty();
	Router.Reset();
	EW::RemoveLogCapture();
}

bool FEnginewrightModule::HandlePost(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Auth
	FString Presented;
	if (const TArray<FString>* H = Request.Headers.Find(TEXT("X-Enginewright-Token")))
	{
		if (H->Num()) { Presented = (*H)[0]; }
	}
	if (const TArray<FString>* A = Request.Headers.Find(TEXT("Authorization")))
	{
		if (A->Num() && (*A)[0].StartsWith(TEXT("Bearer "))) { Presented = (*A)[0].Mid(7); }
	}
	if (Presented.TrimStartAndEnd() != Token)
	{
		OnComplete(Reply(EHttpServerResponseCodes::Denied, TEXT("{\"error\":\"missing or wrong X-Enginewright-Token (see Saved/Enginewright/token)\"}")));
		return true;
	}

	const FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
	const FString Body(Conv.Length(), Conv.Get());
	TSharedPtr<FJsonObject> Msg = EW::ParseObject(Body);
	if (!Msg.IsValid())
	{
		OnComplete(Reply(EHttpServerResponseCodes::BadRequest, RpcError(nullptr, -32700, TEXT("Parse error: send one JSON-RPC object."))));
		return true;
	}

	FString Method;
	Msg->TryGetStringField(TEXT("method"), Method);
	const bool bInit = Method == TEXT("initialize");
	FHttpResultCallback Complete = OnComplete;
	HandleRpc(Msg, [Complete, bInit](const FString& Out)
	{
		if (Out.IsEmpty())
		{
			Complete(Reply(EHttpServerResponseCodes::Accepted, FString()));
			return;
		}
		TUniquePtr<FHttpServerResponse> R = Reply(EHttpServerResponseCodes::Ok, Out);
		if (bInit) { R->Headers.Add(TEXT("Mcp-Session-Id"), { FGuid::NewGuid().ToString(EGuidFormats::Digits) }); }
		Complete(MoveTemp(R));
	});
	return true;
}

void FEnginewrightModule::HandleRpc(const TSharedPtr<FJsonObject>& Msg, TFunction<void(const FString&)> ReplyFn)
{
	const TSharedPtr<FJsonValue> Id = Msg->TryGetField(TEXT("id"));
	FString Method;
	Msg->TryGetStringField(TEXT("method"), Method);
	const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
	TSharedPtr<FJsonObject> Params = Msg->TryGetObjectField(TEXT("params"), ParamsPtr) ? *ParamsPtr : MakeShared<FJsonObject>();

	if (!Id.IsValid() || Id->IsNull())
	{
		ReplyFn(FString()); // Notification (e.g. notifications/initialized): no reply body.
		return;
	}

	if (Method == TEXT("initialize"))
	{
		FString Requested;
		Params->TryGetStringField(TEXT("protocolVersion"), Requested);
		static const TArray<FString> Supported = { TEXT("2025-06-18"), TEXT("2025-03-26"), TEXT("2025-11-25") };
		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("protocolVersion"), Supported.Contains(Requested) ? Requested : Supported[0]);
		TSharedRef<FJsonObject> Caps = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> ToolsCap = MakeShared<FJsonObject>();
		ToolsCap->SetBoolField(TEXT("listChanged"), false);
		Caps->SetObjectField(TEXT("tools"), ToolsCap);
		R->SetObjectField(TEXT("capabilities"), Caps);
		TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
		Info->SetStringField(TEXT("name"), TEXT("enginewright"));
		Info->SetStringField(TEXT("version"), ENGINEWRIGHT_VERSION);
		R->SetObjectField(TEXT("serverInfo"), Info);
		R->SetStringField(TEXT("instructions"), GInstructions);
		ReplyFn(RpcResult(Id, R));
		return;
	}
	if (Method == TEXT("ping"))
	{
		ReplyFn(RpcResult(Id, MakeShared<FJsonObject>()));
		return;
	}
	if (Method == TEXT("tools/list"))
	{
		ReplyFn(RpcResult(Id, ToolsList()));
		return;
	}
	if (Method == TEXT("tools/call"))
	{
		FString Name;
		Params->TryGetStringField(TEXT("name"), Name);
		const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
		TSharedPtr<FJsonObject> Args = Params->TryGetObjectField(TEXT("arguments"), ArgsPtr) ? *ArgsPtr : MakeShared<FJsonObject>();
		CallTool(Name, Args, [Id, ReplyFn](TSharedRef<FJsonObject> Result) { ReplyFn(RpcResult(Id, Result)); });
		return;
	}
	ReplyFn(RpcError(Id, -32601, FString::Printf(TEXT("Method not found: %s"), *Method)));
}

TSharedRef<FJsonObject> FEnginewrightModule::ToolsList() const
{
	TArray<TSharedPtr<FJsonValue>> List;
	List.Add(MakeShared<FJsonValueObject>(ToolEntry(TEXT("search"),
		TEXT("Find Unreal Editor tools by 2-4 plain words (e.g. 'set property', 'console command'). Returns names and one-line summaries."),
		Schema(TEXT(R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"]})")))));
	List.Add(MakeShared<FJsonValueObject>(ToolEntry(TEXT("describe"),
		TEXT("Read one tool's full description and argument schema."),
		Schema(TEXT(R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})")))));
	List.Add(MakeShared<FJsonValueObject>(ToolEntry(TEXT("call"),
		TEXT("Run one tool by name with its arguments object."),
		Schema(TEXT(R"({"type":"object","properties":{"name":{"type":"string"},"args":{"type":"object"}},"required":["name"]})")))));
	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetArrayField(TEXT("tools"), List);
	return R;
}

static FEWResult SearchTools(const FString& Query)
{
	TArray<FString> Words;
	Query.ToLower().ParseIntoArrayWS(Words);
	TArray<TPair<int32, const FEWTool*>> Scored;
	for (const FEWTool& T : FEWRegistry::Get().All())
	{
		const FString Hay = (T.Name + TEXT(" ") + T.Description).ToLower();
		int32 Score = 0;
		for (const FString& W : Words)
		{
			if (T.Name.Contains(W)) { Score += 3; }
			else if (Hay.Contains(W)) { Score += 1; }
		}
		if (Score > 0) { Scored.Emplace(Score, &T); }
	}
	Scored.StableSort([](const auto& A, const auto& B) { return A.Key > B.Key; });
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const auto& S : Scored)
	{
		if (Rows.Num() >= 15) { break; }
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), S.Value->Name);
		Row->SetStringField(TEXT("summary"), FirstSentence(S.Value->Description));
		Row->SetStringField(TEXT("effect"), S.Value->bWrites ? TEXT("write") : TEXT("read"));
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("results"), Rows);
	Data->SetNumberField(TEXT("matched"), Scored.Num());
	return FEWResult::Ok(Data, Rows.Num() ? FString() : TEXT("No match. Try other words, e.g. 'blueprint', 'asset', 'pie', 'material'."));
}

void FEnginewrightModule::CallTool(const FString& Name, const TSharedPtr<FJsonObject>& Args, TFunction<void(TSharedRef<FJsonObject>)> Done) const
{
	auto Finish = [Done](FEWResult Res)
	{
		const TSharedRef<FJsonObject> Body = Res.ToJson();
		TArray<TSharedPtr<FJsonValue>> Content;
		TSharedRef<FJsonObject> Text = MakeShared<FJsonObject>();
		Text->SetStringField(TEXT("type"), TEXT("text"));
		Text->SetStringField(TEXT("text"), EW::ToJsonString(Body));
		Content.Add(MakeShared<FJsonValueObject>(Text));
		for (const TPair<FString, FString>& Img : Res.Images)
		{
			TSharedRef<FJsonObject> I = MakeShared<FJsonObject>();
			I->SetStringField(TEXT("type"), TEXT("image"));
			I->SetStringField(TEXT("mimeType"), Img.Key);
			I->SetStringField(TEXT("data"), Img.Value);
			Content.Add(MakeShared<FJsonValueObject>(I));
		}
		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetArrayField(TEXT("content"), Content);
		R->SetObjectField(TEXT("structuredContent"), Body);
		R->SetBoolField(TEXT("isError"), !Res.bOk);
		Done(R);
	};

	FEWRegistry& Reg = FEWRegistry::Get();
	if (Name == TEXT("search"))
	{
		Finish(SearchTools(EW::Str(Args, TEXT("query"))));
	}
	else if (Name == TEXT("describe"))
	{
		const FString ToolName = EW::Str(Args, TEXT("name"));
		if (const FEWTool* T = Reg.Find(ToolName))
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("name"), T->Name);
			Data->SetStringField(TEXT("description"), T->Description);
			Data->SetStringField(TEXT("effect"), T->bWrites ? TEXT("write") : TEXT("read"));
			Data->SetBoolField(TEXT("waits"), T->IsAsync());
			Data->SetObjectField(TEXT("inputSchema"), T->InputSchema);
			Finish(FEWResult::Ok(Data));
		}
		else
		{
			const FEWTool* None = nullptr;
			Finish(Reg.Check(ToolName, MakeShared<FJsonObject>(), None)); // UNKNOWN_TOOL with suggestions.
		}
	}
	else if (Name == TEXT("call"))
	{
		const TSharedPtr<FJsonObject>* CallArgs = nullptr;
		Reg.CallAsync(EW::Str(Args, TEXT("name")), Args->TryGetObjectField(TEXT("args"), CallArgs) ? *CallArgs : nullptr, Finish);
	}
	else
	{
		Reg.CallAsync(Name, Args, Finish); // Direct call by tool name also works.
	}
}

IMPLEMENT_MODULE(FEnginewrightModule, Enginewright)
