#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EWProps.h"
#include "EWTypes.h"

#include "AssetToolsModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Factories/BlueprintFactory.h"
#include "GameFramework/Actor.h"
#include "IAssetTools.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"

namespace
{
	IAssetTools& AssetTools() { return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get(); }

	UBlueprint* LoadBP(const TSharedPtr<FJsonObject>& Args, FEWResult& OutErr, const TCHAR* Key = TEXT("path"))
	{
		FString Err;
		UBlueprint* BP = EW::LoadAs<UBlueprint>(EW::Str(Args, Key), Err);
		if (!BP) { OutErr = FEWResult::Error(TEXT("BLUEPRINT_NOT_FOUND"), Err); }
		return BP;
	}

	const TCHAR* StatusName(EBlueprintStatus S)
	{
		switch (S)
		{
		case BS_UpToDate: return TEXT("upToDate");
		case BS_UpToDateWithWarnings: return TEXT("upToDateWithWarnings");
		case BS_Error: return TEXT("error");
		case BS_Dirty: return TEXT("dirty");
		default: return TEXT("unknown");
		}
	}

	/** Compiles and returns {status, errors[], warnings[]}. */
	TSharedRef<FJsonObject> Compile(UBlueprint* BP, int32& OutErrors)
	{
		FCompilerResultsLog Log;
		Log.bSilentMode = true;
		Log.SetSourcePath(BP->GetPathName());
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection, &Log);
		TArray<TSharedPtr<FJsonValue>> Errors, Warnings;
		for (const TSharedRef<FTokenizedMessage>& M : Log.Messages)
		{
			const FString Text = M->ToText().ToString();
			if (M->GetSeverity() == EMessageSeverity::Error) { Errors.Add(MakeShared<FJsonValueString>(Text)); }
			else if (M->GetSeverity() == EMessageSeverity::Warning || M->GetSeverity() == EMessageSeverity::PerformanceWarning) { Warnings.Add(MakeShared<FJsonValueString>(Text)); }
		}
		OutErrors = Errors.Num() + (BP->Status == BS_Error && Errors.Num() == 0 ? 1 : 0);
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("path"), BP->GetPathName());
		J->SetStringField(TEXT("status"), StatusName(BP->Status));
		J->SetArrayField(TEXT("errors"), Errors);
		J->SetArrayField(TEXT("warnings"), Warnings);
		return J;
	}

	bool Save(UBlueprint* BP, FEWResult& Res)
	{
		bool bDeferred = false;
		const bool bOk = EW::SaveOrDefer(BP->GetOutermost(), bDeferred);
		if (!bOk) { Res.Warnings.Add(TEXT("Save failed; the change is in memory only.")); }
		return bOk && !bDeferred;
	}

	/** Compile + save + attach the compile report; turns compile errors into an error result. */
	FEWResult Finish(UBlueprint* BP, FEWResult Res, bool bCompile = true)
	{
		if (!Res.Data.IsValid()) { Res.Data = MakeShared<FJsonObject>(); }
		if (bCompile)
		{
			int32 Errors = 0;
			Res.Data->SetObjectField(TEXT("compile"), Compile(BP, Errors));
			if (Errors > 0)
			{
				Res.bOk = false;
				Res.ErrorCode = TEXT("COMPILE_ERROR");
				Res.Message = TEXT("The change was applied but the Blueprint does not compile; see data.compile.errors.");
			}
		}
		Res.Data->SetBoolField(TEXT("saved"), Save(BP, Res));
		return Res;
	}

	TSharedRef<FJsonObject> VariableJson(UBlueprint* BP, const FBPVariableDescription& V)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("name"), V.VarName.ToString());
		J->SetStringField(TEXT("type"), EW::PinTypeToString(V.VarType));
		J->SetStringField(TEXT("category"), V.Category.ToString());
		J->SetBoolField(TEXT("instanceEditable"), !(V.PropertyFlags & CPF_DisableEditOnInstance));
		J->SetStringField(TEXT("default"), V.DefaultValue);
		return J;
	}

	USCS_Node* FindScsNode(UBlueprint* BP, const FString& Name)
	{
		if (!BP->SimpleConstructionScript) { return nullptr; }
		for (USCS_Node* N : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (N && N->GetVariableName().ToString().Equals(Name, ESearchCase::IgnoreCase)) { return N; }
		}
		return nullptr;
	}

	TArray<FString> ScsNames(UBlueprint* BP)
	{
		TArray<FString> Out;
		if (BP->SimpleConstructionScript)
		{
			for (USCS_Node* N : BP->SimpleConstructionScript->GetAllNodes()) { if (N) { Out.Add(N->GetVariableName().ToString()); } }
		}
		return Out;
	}

	// ---- graphs --------------------------------------------------------------------------------

	UEdGraph* FindGraph(UBlueprint* BP, const FString& Name, TArray<FString>& OutNames)
	{
		TArray<UEdGraph*> All;
		BP->GetAllGraphs(All);
		const FString Want = Name.IsEmpty() ? TEXT("EventGraph") : Name;
		for (UEdGraph* G : All)
		{
			OutNames.Add(G->GetName());
			if (G->GetName().Equals(Want, ESearchCase::IgnoreCase)) { return G; }
		}
		return nullptr;
	}

	FString NodeId(const UEdGraphNode* N) { return N->NodeGuid.ToString(EGuidFormats::Digits).Left(12); }

	UEdGraphNode* FindNode(UEdGraph* G, const FString& Id)
	{
		for (UEdGraphNode* N : G->Nodes)
		{
			if (!N) { continue; }
			if (NodeId(N).Equals(Id, ESearchCase::IgnoreCase) || N->NodeGuid.ToString(EGuidFormats::Digits).Equals(Id, ESearchCase::IgnoreCase) || N->GetName().Equals(Id, ESearchCase::IgnoreCase))
			{
				return N;
			}
		}
		return nullptr;
	}

	FString PinLabel(const UEdGraphPin* P) { return P->PinName.ToString(); }

	/** Finds a pin by name, friendly alias (exec/then/in/out/result) or index (#2). */
	UEdGraphPin* FindPin(UEdGraphNode* N, const FString& InName, EEdGraphPinDirection Dir)
	{
		FString Name = InName.TrimStartAndEnd();
		TArray<UEdGraphPin*> Pins;
		for (UEdGraphPin* P : N->Pins) { if (P && P->Direction == Dir && !P->bHidden) { Pins.Add(P); } }
		if (Name.StartsWith(TEXT("#")))
		{
			const int32 Index = FCString::Atoi(*Name.RightChop(1));
			return Pins.IsValidIndex(Index) ? Pins[Index] : nullptr;
		}
		for (UEdGraphPin* P : Pins)
		{
			if (P->PinName.ToString().Equals(Name, ESearchCase::IgnoreCase) || P->GetDisplayName().ToString().Equals(Name, ESearchCase::IgnoreCase)) { return P; }
		}
		const FString L = Name.ToLower();
		const bool bExecAlias = L == TEXT("exec") || L == TEXT("then") || L == TEXT("execute") || L == TEXT("in") || L == TEXT("out");
		const bool bValueAlias = L == TEXT("result") || L == TEXT("value") || L == TEXT("returnvalue") || L == TEXT("output");
		for (UEdGraphPin* P : Pins)
		{
			const bool bExec = P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
			if (bExecAlias && bExec) { return P; }
			if (bValueAlias && !bExec) { return P; }
		}
		return nullptr;
	}

	TSharedRef<FJsonObject> NodeJson(UEdGraphNode* N)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("id"), NodeId(N));
		J->SetStringField(TEXT("class"), N->GetClass()->GetName());
		J->SetStringField(TEXT("title"), N->GetNodeTitle(ENodeTitleType::ListView).ToString());
		J->SetArrayField(TEXT("position"), { MakeShared<FJsonValueNumber>(N->NodePosX), MakeShared<FJsonValueNumber>(N->NodePosY) });
		TArray<TSharedPtr<FJsonValue>> Pins;
		for (UEdGraphPin* P : N->Pins)
		{
			if (!P || P->bHidden) { continue; }
			TSharedRef<FJsonObject> PJ = MakeShared<FJsonObject>();
			PJ->SetStringField(TEXT("name"), PinLabel(P));
			PJ->SetStringField(TEXT("dir"), P->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));
			PJ->SetStringField(TEXT("type"), P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec ? TEXT("exec") : EW::PinTypeToString(P->PinType));
			if (P->Direction == EGPD_Input && P->LinkedTo.Num() == 0 && !P->DefaultValue.IsEmpty()) { PJ->SetStringField(TEXT("default"), P->DefaultValue); }
			if (P->Direction == EGPD_Input && P->DefaultObject) { PJ->SetStringField(TEXT("default"), P->DefaultObject->GetPathName()); }
			TArray<TSharedPtr<FJsonValue>> Links;
			for (UEdGraphPin* L : P->LinkedTo)
			{
				if (L && L->GetOwningNode()) { Links.Add(MakeShared<FJsonValueString>(NodeId(L->GetOwningNode()) + TEXT(".") + PinLabel(L))); }
			}
			if (Links.Num()) { PJ->SetArrayField(TEXT("links"), Links); }
			Pins.Add(MakeShared<FJsonValueObject>(PJ));
		}
		J->SetArrayField(TEXT("pins"), Pins);
		return J;
	}

	UFunction* FindFunctionByName(UBlueprint* BP, const FString& InName, FString& OutError)
	{
		FString ClassName, FuncName = InName;
		if (!InName.Split(TEXT("::"), &ClassName, &FuncName)) { InName.Split(TEXT("."), &ClassName, &FuncName); }
		if (FuncName.IsEmpty()) { FuncName = InName; ClassName.Reset(); }
		TArray<UClass*> Search;
		if (!ClassName.IsEmpty())
		{
			UClass* C = EW::FindClass(ClassName, OutError);
			if (!C) { return nullptr; }
			Search.Add(C);
		}
		else
		{
			if (BP->SkeletonGeneratedClass) { Search.Add(BP->SkeletonGeneratedClass); }
			Search.Add(BP->ParentClass);
			Search.Add(UKismetSystemLibrary::StaticClass());
			Search.Add(UKismetMathLibrary::StaticClass());
			Search.Add(UGameplayStatics::StaticClass());
			Search.Add(UKismetStringLibrary::StaticClass());
		}
		TArray<FString> Names;
		for (UClass* C : Search)
		{
			if (!C) { continue; }
			if (UFunction* F = C->FindFunctionByName(FName(*FuncName))) { return F; }
			for (TFieldIterator<UFunction> It(C); It; ++It)
			{
				if (It->GetName().Equals(FuncName, ESearchCase::IgnoreCase)) { return *It; }
				if (It->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure)) { Names.Add(It->GetName()); }
			}
		}
		const TArray<FString> Sug = EW::Suggest(FuncName, Names);
		OutError = FString::Printf(TEXT("No Blueprint-callable function '%s'. Use Class.Function for other classes.%s"), *InName,
			Sug.Num() ? *FString::Printf(TEXT(" Did you mean: %s?"), *FString::Join(Sug, TEXT(", "))) : TEXT(""));
		return nullptr;
	}

	FEWResult Bad(const FString& Code, const FString& Msg) { return FEWResult::Error(Code, Msg); }
}

void RegisterBlueprintTools(FEWRegistry& R)
{
	R.Register(TEXT("blueprint_create"),
		TEXT("Create a Blueprint class asset from a parent class (Actor, Pawn, Character, ActorComponent, a C++ class or another Blueprint), compile and save it."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string","description":"New asset path, e.g. /Game/Blueprints/BP_Door."},
			"parent":{"type":"string","description":"Parent class: short name, /Script/Module.Class or a Blueprint path. Default Actor."}},
			"required":["path"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Folder, Name, Err;
			if (!EW::SplitAssetPath(EW::Str(Args, TEXT("path")), Folder, Name, Err)) { return Bad(TEXT("INVALID_PATH"), Err); }
			if (EW::AssetPathTaken(Folder / Name))
			{
				return Bad(TEXT("ASSET_EXISTS"), FString::Printf(TEXT("'%s' already exists."), *(Folder / Name)));
			}
			const FString ParentName = Args->HasField(TEXT("parent")) ? EW::Str(Args, TEXT("parent")) : TEXT("Actor");
			TArray<FString> Sug;
			UClass* Parent = EW::FindClass(ParentName, Err, &Sug);
			if (!Parent) { return FEWResult::Error(TEXT("UNKNOWN_CLASS"), Err, Sug); }
			if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(Parent))
			{
				return Bad(TEXT("INVALID_PARENT"), FString::Printf(TEXT("A Blueprint cannot derive from %s."), *Parent->GetName()));
			}
			UBlueprintFactory* F = NewObject<UBlueprintFactory>();
			F->ParentClass = Parent;
			F->bSkipClassPicker = true;
			UBlueprint* BP = Cast<UBlueprint>(AssetTools().CreateAsset(Name, Folder, UBlueprint::StaticClass(), F));
			if (!BP) { return Bad(TEXT("CREATE_FAILED"), TEXT("The editor could not create the Blueprint.")); }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("path"), BP->GetPathName());
			Data->SetStringField(TEXT("class"), BP->GeneratedClass ? BP->GeneratedClass->GetPathName() : FString());
			Data->SetStringField(TEXT("parent"), Parent->GetPathName());
			return Finish(BP, FEWResult::Ok(Data));
		}, /*bTransactional*/ false);

	R.Register(TEXT("blueprint_info"),
		TEXT("Describe a Blueprint: parent class, compile status, variables (type, default, category), components, graphs and functions."),
		TEXT(R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UBlueprint* BP = LoadBP(Args, Err);
			if (!BP) { return Err; }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("path"), BP->GetPathName());
			Data->SetStringField(TEXT("parent"), BP->ParentClass ? BP->ParentClass->GetPathName() : FString());
			Data->SetStringField(TEXT("class"), BP->GeneratedClass ? BP->GeneratedClass->GetPathName() : FString());
			Data->SetStringField(TEXT("status"), StatusName(BP->Status));
			TArray<TSharedPtr<FJsonValue>> Vars;
			for (const FBPVariableDescription& V : BP->NewVariables) { Vars.Add(MakeShared<FJsonValueObject>(VariableJson(BP, V))); }
			Data->SetArrayField(TEXT("variables"), Vars);
			TArray<TSharedPtr<FJsonValue>> Comps;
			if (BP->SimpleConstructionScript)
			{
				for (USCS_Node* N : BP->SimpleConstructionScript->GetAllNodes())
				{
					if (!N) { continue; }
					TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
					C->SetStringField(TEXT("name"), N->GetVariableName().ToString());
					C->SetStringField(TEXT("class"), N->ComponentClass ? N->ComponentClass->GetName() : FString());
					USCS_Node* Parent = BP->SimpleConstructionScript->FindParentNode(N);
					C->SetStringField(TEXT("parent"), Parent ? Parent->GetVariableName().ToString() : FString());
					if (N->ComponentTemplate) { C->SetStringField(TEXT("template"), N->ComponentTemplate->GetPathName()); }
					Comps.Add(MakeShared<FJsonValueObject>(C));
				}
			}
			Data->SetArrayField(TEXT("components"), Comps);
			TArray<UEdGraph*> Graphs;
			BP->GetAllGraphs(Graphs);
			TArray<TSharedPtr<FJsonValue>> GraphList;
			for (UEdGraph* G : Graphs)
			{
				TSharedRef<FJsonObject> GJ = MakeShared<FJsonObject>();
				GJ->SetStringField(TEXT("name"), G->GetName());
				GJ->SetNumberField(TEXT("nodes"), G->Nodes.Num());
				GJ->SetStringField(TEXT("kind"), BP->FunctionGraphs.Contains(G) ? TEXT("function") : BP->UbergraphPages.Contains(G) ? TEXT("event") : TEXT("other"));
				GraphList.Add(MakeShared<FJsonValueObject>(GJ));
			}
			Data->SetArrayField(TEXT("graphs"), GraphList);
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("blueprint_compile"),
		TEXT("Compile Blueprints and report errors and warnings per Blueprint. Pass paths, or a folder to compile every Blueprint under it."),
		TEXT(R"({"type":"object","properties":{
			"paths":{"type":"array","items":{"type":"string"}},
			"folder":{"type":"string","description":"Compile all Blueprints under this folder, e.g. /Game."},
			"save":{"type":"boolean","description":"Save after a clean compile (default false)."}}})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			TArray<FString> Paths = EW::StrList(Args, TEXT("paths"));
			const FString Folder = EW::Str(Args, TEXT("folder"));
			if (!Folder.IsEmpty())
			{
				Paths.Append(EW::FindAssets(Folder, UBlueprint::StaticClass()));
			}
			if (Paths.Num() == 0) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("Pass paths or a folder that has Blueprints.")); }
			const bool bSave = EW::Bool(Args, TEXT("save"), false);
			TArray<TSharedPtr<FJsonValue>> Results;
			int32 Failed = 0, Total = 0;
			for (const FString& P : Paths)
			{
				FString Err;
				UBlueprint* BP = EW::LoadAs<UBlueprint>(P, Err);
				if (!BP) { return Bad(TEXT("BLUEPRINT_NOT_FOUND"), Err); }
				int32 Errors = 0;
				TSharedRef<FJsonObject> J = Compile(BP, Errors);
				++Total;
				if (Errors) { ++Failed; }
				else if (bSave) { bool bDeferred = false; J->SetBoolField(TEXT("saved"), EW::SaveOrDefer(BP->GetOutermost(), bDeferred)); }
				Results.Add(MakeShared<FJsonValueObject>(J));
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("results"), Results);
			Data->SetNumberField(TEXT("compiled"), Total);
			Data->SetNumberField(TEXT("failed"), Failed);
			FEWResult Res = Failed ? FEWResult::Error(TEXT("COMPILE_ERROR"), FString::Printf(TEXT("%d of %d Blueprint(s) have compile errors."), Failed, Total)) : FEWResult::Ok();
			Res.Data = Data;
			return Res;
		}, /*bTransactional*/ false);

	R.Register(TEXT("blueprint_add_variables"),
		TEXT("Add member variables to a Blueprint: [{name, type, default, category, instanceEditable}]. All are checked first (name clash, type); then added, compiled, read back from the class defaults, and saved."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string"},
			"variables":{"type":"array","items":{"type":"object"},"description":"type uses the struct_create type strings; default is the value as text (e.g. '1.5', 'true', '(X=1,Y=2,Z=3)')."}},
			"required":["path","variables"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UBlueprint* BP = LoadBP(Args, Err);
			if (!BP) { return Err; }
			const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
			Args->TryGetArrayField(TEXT("variables"), List);
			if (!List || List->Num() == 0) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("variables must list at least one variable.")); }
			struct FVar { FName Name; FEdGraphPinType Type; FString Default, Category; bool bEditable = true; };
			TArray<FVar> Plan;
			static const TArray<FString> Keys = { TEXT("name"), TEXT("type"), TEXT("default"), TEXT("category"), TEXT("instanceEditable") };
			for (int32 i = 0; i < List->Num(); ++i)
			{
				const TSharedPtr<FJsonObject>* O = nullptr;
				if (!(*List)[i]->TryGetObject(O)) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("variables[%d] is not an object."), i)); }
				for (const auto& KV : (*O)->Values)
				{
					if (!Keys.Contains(FString(*KV.Key)))
					{
						return FEWResult::Error(TEXT("UNKNOWN_ARGUMENT"), FString::Printf(TEXT("variables[%d] has unknown key '%s'."), i, *FString(*KV.Key)), EW::Suggest(FString(*KV.Key), Keys));
					}
				}
				FVar V;
				FString Name, Type, TypeErr;
				(*O)->TryGetStringField(TEXT("name"), Name);
				(*O)->TryGetStringField(TEXT("type"), Type);
				(*O)->TryGetStringField(TEXT("default"), V.Default);
				(*O)->TryGetStringField(TEXT("category"), V.Category);
				(*O)->TryGetBoolField(TEXT("instanceEditable"), V.bEditable);
				if (Name.IsEmpty() || Type.IsEmpty()) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("variables[%d] needs name and type."), i)); }
				V.Name = FName(*Name);
				if (FBlueprintEditorUtils::FindNewVariableIndex(BP, V.Name) != INDEX_NONE || FindFProperty<FProperty>(BP->ParentClass, V.Name) || Plan.ContainsByPredicate([&](const FVar& X) { return X.Name == V.Name; }))
				{
					return Bad(TEXT("NAME_IN_USE"), FString::Printf(TEXT("'%s' already exists on this Blueprint or its parent. Nothing was added."), *Name));
				}
				if (!EW::ParsePinType(Type, V.Type, TypeErr)) { return Bad(TEXT("INVALID_TYPE"), FString::Printf(TEXT("variables[%d] (%s): %s Nothing was added."), i, *Name, *TypeErr)); }
				if (UEnum* Enum = Cast<UEnum>(V.Type.PinSubCategoryObject.Get()); Enum && !V.Default.IsEmpty() && V.Type.ContainerType == EPinContainerType::None)
				{
					// Pin defaults hold the internal enumerator name; accept the display name or the short name too.
					FString Internal;
					TArray<FString> Known;
					for (int32 e = 0; e < Enum->NumEnums() - 1; ++e)
					{
						const FString Short = Enum->GetNameStringByIndex(e), Display = Enum->GetDisplayNameTextByIndex(e).ToString();
						Known.Add(Display);
						if (V.Default.Equals(Short, ESearchCase::IgnoreCase) || V.Default.Equals(Display, ESearchCase::IgnoreCase)) { Internal = Short; }
					}
					if (Internal.IsEmpty())
					{
						return FEWResult::Error(TEXT("INVALID_VALUE"), FString::Printf(TEXT("variables[%d] (%s): '%s' is not a value of %s. Nothing was added."), i, *Name, *V.Default, *Enum->GetName()), EW::Suggest(V.Default, Known));
					}
					V.Default = Internal;
				}
				Plan.Add(V);
			}
			for (const FVar& V : Plan)
			{
				if (!FBlueprintEditorUtils::AddMemberVariable(BP, V.Name, V.Type, V.Default))
				{
					return Bad(TEXT("WRITE_FAILED"), FString::Printf(TEXT("Could not add '%s' (earlier variables of this call were added)."), *V.Name.ToString()));
				}
				FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(BP, V.Name, !V.bEditable);
				if (!V.Category.IsEmpty()) { FBlueprintEditorUtils::SetBlueprintVariableCategory(BP, V.Name, nullptr, FText::FromString(V.Category)); }
			}
			FEWResult Res = Finish(BP, FEWResult::Ok());
			if (!Res.bOk) { return Res; }
			// Read back from the generated class defaults.
			UObject* CDO = BP->GeneratedClass ? BP->GeneratedClass->GetDefaultObject() : nullptr;
			TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
			for (const FVar& V : Plan)
			{
				FProperty* P = CDO ? FindFProperty<FProperty>(BP->GeneratedClass, V.Name) : nullptr;
				if (!P) { return Bad(TEXT("READBACK_MISMATCH"), FString::Printf(TEXT("'%s' is missing from the compiled class."), *V.Name.ToString())); }
				TSharedPtr<FJsonObject> One;
				EW::ReadValues(BP->GeneratedClass, CDO, { V.Name.ToString() }, One);
				Values->SetField(V.Name.ToString(), One->TryGetField(V.Name.ToString()));
			}
			Res.Data->SetObjectField(TEXT("defaults"), Values);
			return Res;
		}, /*bTransactional*/ false);

	R.Register(TEXT("blueprint_remove_variables"),
		TEXT("Remove member variables from a Blueprint (and the nodes that use them), compile and save."),
		TEXT(R"({"type":"object","properties":{"path":{"type":"string"},"names":{"type":"array","items":{"type":"string"}}},"required":["path","names"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UBlueprint* BP = LoadBP(Args, Err);
			if (!BP) { return Err; }
			const TArray<FString> Names = EW::StrList(Args, TEXT("names"));
			TArray<FString> Existing;
			for (const FBPVariableDescription& V : BP->NewVariables) { Existing.Add(V.VarName.ToString()); }
			for (const FString& N : Names)
			{
				if (FBlueprintEditorUtils::FindNewVariableIndex(BP, FName(*N)) == INDEX_NONE)
				{
					return FEWResult::Error(TEXT("UNKNOWN_VARIABLE"), FString::Printf(TEXT("No variable '%s'. Nothing was removed."), *N), EW::Suggest(N, Existing));
				}
			}
			for (const FString& N : Names) { FBlueprintEditorUtils::RemoveMemberVariable(BP, FName(*N)); }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("removed"), Names.Num());
			return Finish(BP, FEWResult::Ok(Data));
		}, /*bTransactional*/ false);

	R.Register(TEXT("blueprint_add_component"),
		TEXT("Add a component to an Actor Blueprint (e.g. StaticMeshComponent, PointLightComponent), optionally under a parent component and with property values checked like object_set; compile and save."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string"},
			"class":{"type":"string","description":"Component class, e.g. StaticMeshComponent."},
			"name":{"type":"string","description":"Component name (variable name)."},
			"parent":{"type":"string","description":"Parent scene component name; default is the root."},
			"values":{"type":"object","description":"Property values for the component template, e.g. {\"StaticMesh\": \"/Engine/BasicShapes/Cube.Cube\"}."}},
			"required":["path","class","name"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UBlueprint* BP = LoadBP(Args, Err);
			if (!BP) { return Err; }
			if (!BP->SimpleConstructionScript) { return Bad(TEXT("NOT_ACTOR_BLUEPRINT"), TEXT("Only Actor Blueprints have components.")); }
			FString CErr;
			TArray<FString> Sug;
			UClass* C = EW::FindClass(EW::Str(Args, TEXT("class")), CErr, &Sug);
			if (!C) { return FEWResult::Error(TEXT("UNKNOWN_CLASS"), CErr, Sug); }
			if (!C->IsChildOf(UActorComponent::StaticClass()) || C->HasAnyClassFlags(CLASS_Abstract))
			{
				return Bad(TEXT("INVALID_CLASS"), FString::Printf(TEXT("%s is not a concrete component class."), *C->GetName()));
			}
			const FString Name = EW::Str(Args, TEXT("name"));
			if (FindScsNode(BP, Name) || FindFProperty<FProperty>(BP->ParentClass, FName(*Name)) || FBlueprintEditorUtils::FindNewVariableIndex(BP, FName(*Name)) != INDEX_NONE)
			{
				return Bad(TEXT("NAME_IN_USE"), FString::Printf(TEXT("'%s' is already used on this Blueprint."), *Name));
			}
			USCS_Node* Parent = nullptr;
			const FString ParentName = EW::Str(Args, TEXT("parent"));
			if (!ParentName.IsEmpty())
			{
				Parent = FindScsNode(BP, ParentName);
				if (!Parent) { return FEWResult::Error(TEXT("UNKNOWN_COMPONENT"), FString::Printf(TEXT("No component '%s' in this Blueprint."), *ParentName), EW::Suggest(ParentName, ScsNames(BP))); }
				if (!C->IsChildOf(USceneComponent::StaticClass())) { return Bad(TEXT("INVALID_PARENT"), TEXT("Only scene components can have a parent.")); }
			}
			const TSharedPtr<FJsonObject>* Values = nullptr;
			Args->TryGetObjectField(TEXT("values"), Values);

			USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
			USCS_Node* Node = SCS->CreateNode(C, FName(*Name));
			if (Values && (*Values)->Values.Num())
			{
				FEWResult W = EW::WriteValues(C, Node->ComponentTemplate, Node->ComponentTemplate, *Values);
				if (!W.bOk) { return W; } // Node is not added yet: nothing changed.
			}
			if (Parent) { Parent->AddChildNode(Node); }
			else if (C->IsChildOf(USceneComponent::StaticClass()) && SCS->GetDefaultSceneRootNode() && SCS->GetRootNodes().Contains(SCS->GetDefaultSceneRootNode()))
			{
				SCS->GetDefaultSceneRootNode()->AddChildNode(Node);
			}
			else { SCS->AddNode(Node); }
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
			FEWResult Res = Finish(BP, FEWResult::Ok(Data));
			USCS_Node* After = FindScsNode(BP, Name);
			if (!After || !After->ComponentTemplate) { return Bad(TEXT("READBACK_MISMATCH"), TEXT("The component is missing after compile.")); }
			Res.Data->SetStringField(TEXT("template"), After->ComponentTemplate->GetPathName());
			return Res;
		}, /*bTransactional*/ false);

	R.Register(TEXT("blueprint_remove_component"),
		TEXT("Remove a component added in this Blueprint (children move up to its parent), compile and save."),
		TEXT(R"({"type":"object","properties":{"path":{"type":"string"},"name":{"type":"string"}},"required":["path","name"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UBlueprint* BP = LoadBP(Args, Err);
			if (!BP) { return Err; }
			const FString Name = EW::Str(Args, TEXT("name"));
			USCS_Node* Node = FindScsNode(BP, Name);
			if (!Node) { return FEWResult::Error(TEXT("UNKNOWN_COMPONENT"), FString::Printf(TEXT("No component '%s' in this Blueprint."), *Name), EW::Suggest(Name, ScsNames(BP))); }
			BP->SimpleConstructionScript->RemoveNodeAndPromoteChildren(Node);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
			return Finish(BP, FEWResult::Ok());
		}, /*bTransactional*/ false);

	R.Register(TEXT("blueprint_graph_read"),
		TEXT("Read a Blueprint graph: nodes (id, class, title, position) and their pins (name, direction, type, default, links as nodeId.pin)."),
		TEXT(R"({"type":"object","properties":{"path":{"type":"string"},"graph":{"type":"string","description":"Graph name, default EventGraph."}},"required":["path"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UBlueprint* BP = LoadBP(Args, Err);
			if (!BP) { return Err; }
			TArray<FString> Names;
			UEdGraph* G = FindGraph(BP, EW::Str(Args, TEXT("graph")), Names);
			if (!G) { return FEWResult::Error(TEXT("UNKNOWN_GRAPH"), TEXT("No graph by that name."), EW::Suggest(EW::Str(Args, TEXT("graph")), Names)); }
			TArray<TSharedPtr<FJsonValue>> Nodes;
			for (UEdGraphNode* N : G->Nodes) { if (N) { Nodes.Add(MakeShared<FJsonValueObject>(NodeJson(N))); } }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("graph"), G->GetName());
			Data->SetArrayField(TEXT("nodes"), Nodes);
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("blueprint_add_node"),
		TEXT("Add a node to a Blueprint graph. kind: event (BeginPlay, Tick or any overridable event), custom_event, call (a function: PrintString, Class.Function, or a function of this Blueprint), get / set (a variable), branch. Returns the node id and pins; wire it with blueprint_connect."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string"},
			"graph":{"type":"string","description":"Default EventGraph."},
			"kind":{"type":"string","enum":["event","custom_event","call","get","set","branch"]},
			"name":{"type":"string","description":"Event, function or variable name (not needed for branch)."},
			"position":{"type":"array","items":{"type":"number"},"description":"[x, y]"}},
			"required":["path","kind"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UBlueprint* BP = LoadBP(Args, Err);
			if (!BP) { return Err; }
			TArray<FString> GraphNames;
			UEdGraph* G = FindGraph(BP, EW::Str(Args, TEXT("graph")), GraphNames);
			if (!G) { return FEWResult::Error(TEXT("UNKNOWN_GRAPH"), TEXT("No graph by that name."), EW::Suggest(EW::Str(Args, TEXT("graph")), GraphNames)); }
			const FString Kind = EW::Str(Args, TEXT("kind")).ToLower();
			const FString Name = EW::Str(Args, TEXT("name"));
			if (Kind != TEXT("branch") && Name.IsEmpty()) { return Bad(TEXT("MISSING_ARGUMENT"), FString::Printf(TEXT("kind '%s' needs name."), *Kind)); }
			int32 X = 0, Y = 0;
			const TArray<TSharedPtr<FJsonValue>>* Pos = nullptr;
			if (Args->TryGetArrayField(TEXT("position"), Pos) && Pos->Num() == 2) { X = (*Pos)[0]->AsNumber(); Y = (*Pos)[1]->AsNumber(); }

			UEdGraphNode* Made = nullptr;
			if (Kind == TEXT("event"))
			{
				UFunction* F = nullptr;
				for (const FString& Candidate : { Name, TEXT("Receive") + Name })
				{
					F = BP->ParentClass ? BP->ParentClass->FindFunctionByName(FName(*Candidate)) : nullptr;
					if (F && F->HasAnyFunctionFlags(FUNC_BlueprintEvent)) { break; }
					F = nullptr;
				}
				if (!F)
				{
					TArray<FString> Events;
					for (TFieldIterator<UFunction> It(BP->ParentClass); It; ++It) { if (It->HasAnyFunctionFlags(FUNC_BlueprintEvent)) { Events.Add(It->GetName()); } }
					return FEWResult::Error(TEXT("UNKNOWN_EVENT"), FString::Printf(TEXT("%s has no overridable event '%s'."), *BP->ParentClass->GetName(), *Name), EW::Suggest(Name, Events));
				}
				if (UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(BP, F->GetOwnerClass(), F->GetFName()))
				{
					// New Blueprints already hold BeginPlay/Tick as greyed-out default nodes: reuse it.
					Existing->SetEnabledState(ENodeEnabledState::Enabled, false);
					FEWResult Res = FEWResult::Ok(NodeJson(Existing), FString::Printf(TEXT("The graph already had %s; this is that node."), *F->GetName()));
					Res.Data->SetBoolField(TEXT("existing"), true);
					return Finish(BP, Res, /*bCompile*/ false);
				}
				FGraphNodeCreator<UK2Node_Event> Creator(*G);
				UK2Node_Event* N = Creator.CreateNode();
				N->EventReference.SetExternalMember(F->GetFName(), F->GetOwnerClass());
				N->bOverrideFunction = true;
				N->NodePosX = X; N->NodePosY = Y;
				Creator.Finalize();
				Made = N;
			}
			else if (Kind == TEXT("custom_event"))
			{
				FGraphNodeCreator<UK2Node_CustomEvent> Creator(*G);
				UK2Node_CustomEvent* N = Creator.CreateNode();
				N->CustomFunctionName = FName(*Name);
				N->NodePosX = X; N->NodePosY = Y;
				Creator.Finalize();
				Made = N;
			}
			else if (Kind == TEXT("call"))
			{
				FString FErr;
				UFunction* F = FindFunctionByName(BP, Name, FErr);
				if (!F) { return Bad(TEXT("UNKNOWN_FUNCTION"), FErr); }
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*G);
				UK2Node_CallFunction* N = Creator.CreateNode();
				N->SetFromFunction(F);
				N->NodePosX = X; N->NodePosY = Y;
				Creator.Finalize();
				Made = N;
			}
			else if (Kind == TEXT("get") || Kind == TEXT("set"))
			{
				const FName Var(*Name);
				const bool bOwn = FBlueprintEditorUtils::FindNewVariableIndex(BP, Var) != INDEX_NONE;
				FProperty* Inherited = BP->ParentClass ? FindFProperty<FProperty>(BP->ParentClass, Var) : nullptr;
				if (!bOwn && !Inherited)
				{
					TArray<FString> Vars;
					for (const FBPVariableDescription& V : BP->NewVariables) { Vars.Add(V.VarName.ToString()); }
					return FEWResult::Error(TEXT("UNKNOWN_VARIABLE"), FString::Printf(TEXT("No variable '%s'."), *Name), EW::Suggest(Name, Vars));
				}
				if (Kind == TEXT("get"))
				{
					FGraphNodeCreator<UK2Node_VariableGet> Creator(*G);
					UK2Node_VariableGet* N = Creator.CreateNode();
					N->VariableReference.SetSelfMember(Var);
					N->NodePosX = X; N->NodePosY = Y;
					Creator.Finalize();
					Made = N;
				}
				else
				{
					FGraphNodeCreator<UK2Node_VariableSet> Creator(*G);
					UK2Node_VariableSet* N = Creator.CreateNode();
					N->VariableReference.SetSelfMember(Var);
					N->NodePosX = X; N->NodePosY = Y;
					Creator.Finalize();
					Made = N;
				}
			}
			else if (Kind == TEXT("branch"))
			{
				FGraphNodeCreator<UK2Node_IfThenElse> Creator(*G);
				UK2Node_IfThenElse* N = Creator.CreateNode();
				N->NodePosX = X; N->NodePosY = Y;
				Creator.Finalize();
				Made = N;
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
			FEWResult Res = Finish(BP, FEWResult::Ok(NodeJson(Made)), /*bCompile*/ false);
			return Res;
		}, /*bTransactional*/ false);

	R.Register(TEXT("blueprint_connect"),
		TEXT("Connect two pins in a Blueprint graph: from an output pin to an input pin, each as nodeId.pin (pin names from blueprint_graph_read; aliases exec/then and #index work). The connection is checked by the graph schema and read back."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string"},
			"graph":{"type":"string"},
			"from":{"type":"string","description":"Output pin, e.g. 3F2A9C10B4D1.then"},
			"to":{"type":"string","description":"Input pin, e.g. 77AB12CD9E00.InString"}},
			"required":["path","from","to"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UBlueprint* BP = LoadBP(Args, Err);
			if (!BP) { return Err; }
			TArray<FString> GraphNames;
			UEdGraph* G = FindGraph(BP, EW::Str(Args, TEXT("graph")), GraphNames);
			if (!G) { return FEWResult::Error(TEXT("UNKNOWN_GRAPH"), TEXT("No graph by that name."), EW::Suggest(EW::Str(Args, TEXT("graph")), GraphNames)); }
			UEdGraphPin* Pins[2] = { nullptr, nullptr };
			const FString Ends[2] = { EW::Str(Args, TEXT("from")), EW::Str(Args, TEXT("to")) };
			for (int32 i = 0; i < 2; ++i)
			{
				FString NodePart, PinPart;
				if (!Ends[i].Split(TEXT("."), &NodePart, &PinPart)) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("'%s' must be nodeId.pin."), *Ends[i])); }
				UEdGraphNode* N = FindNode(G, NodePart);
				if (!N) { return Bad(TEXT("UNKNOWN_NODE"), FString::Printf(TEXT("No node '%s' in %s (ids from blueprint_graph_read)."), *NodePart, *G->GetName())); }
				Pins[i] = FindPin(N, PinPart, i == 0 ? EGPD_Output : EGPD_Input);
				if (!Pins[i])
				{
					TArray<FString> PinNames;
					for (UEdGraphPin* P : N->Pins) { if (P && P->Direction == (i == 0 ? EGPD_Output : EGPD_Input) && !P->bHidden) { PinNames.Add(PinLabel(P)); } }
					return FEWResult::Error(TEXT("UNKNOWN_PIN"), FString::Printf(TEXT("Node %s has no %s pin '%s'. %s pins: %s"), *NodePart, i == 0 ? TEXT("output") : TEXT("input"), *PinPart,
						i == 0 ? TEXT("Output") : TEXT("Input"), *FString::Join(PinNames, TEXT(", "))), EW::Suggest(PinPart, PinNames));
				}
			}
			const UEdGraphSchema* Schema = G->GetSchema();
			const FPinConnectionResponse Resp = Schema->CanCreateConnection(Pins[0], Pins[1]);
			if (Resp.Response == CONNECT_RESPONSE_DISALLOW) { return Bad(TEXT("CONNECTION_REFUSED"), Resp.Message.ToString()); }
			if (!Schema->TryCreateConnection(Pins[0], Pins[1]) || !Pins[0]->LinkedTo.Contains(Pins[1]))
			{
				return Bad(TEXT("CONNECTION_REFUSED"), FString::Printf(TEXT("The pins did not connect. %s"), *Resp.Message.ToString()));
			}
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("from"), NodeId(Pins[0]->GetOwningNode()) + TEXT(".") + PinLabel(Pins[0]));
			Data->SetStringField(TEXT("to"), NodeId(Pins[1]->GetOwningNode()) + TEXT(".") + PinLabel(Pins[1]));
			if (!Resp.Message.IsEmpty()) { Data->SetStringField(TEXT("note"), Resp.Message.ToString()); }
			return Finish(BP, FEWResult::Ok(Data), /*bCompile*/ false);
		}, /*bTransactional*/ false);

	R.Register(TEXT("blueprint_set_pin"),
		TEXT("Set the default value of an unconnected input pin (text form, e.g. 'Hello', '2.5', 'true', '(X=1,Y=0,Z=0)' or an asset path for object pins). The schema checks the value and it is read back."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string"},"graph":{"type":"string"},
			"pin":{"type":"string","description":"nodeId.pin"},
			"value":{"type":"string"}},
			"required":["path","pin","value"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UBlueprint* BP = LoadBP(Args, Err);
			if (!BP) { return Err; }
			TArray<FString> GraphNames;
			UEdGraph* G = FindGraph(BP, EW::Str(Args, TEXT("graph")), GraphNames);
			if (!G) { return FEWResult::Error(TEXT("UNKNOWN_GRAPH"), TEXT("No graph by that name."), EW::Suggest(EW::Str(Args, TEXT("graph")), GraphNames)); }
			FString NodePart, PinPart;
			if (!EW::Str(Args, TEXT("pin")).Split(TEXT("."), &NodePart, &PinPart)) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("pin must be nodeId.pin.")); }
			UEdGraphNode* N = FindNode(G, NodePart);
			if (!N) { return Bad(TEXT("UNKNOWN_NODE"), FString::Printf(TEXT("No node '%s'."), *NodePart)); }
			UEdGraphPin* P = FindPin(N, PinPart, EGPD_Input);
			if (!P)
			{
				TArray<FString> PinNames;
				for (UEdGraphPin* X : N->Pins) { if (X && X->Direction == EGPD_Input && !X->bHidden) { PinNames.Add(PinLabel(X)); } }
				return FEWResult::Error(TEXT("UNKNOWN_PIN"), FString::Printf(TEXT("No input pin '%s'."), *PinPart), EW::Suggest(PinPart, PinNames));
			}
			if (P->LinkedTo.Num()) { return Bad(TEXT("PIN_CONNECTED"), TEXT("The pin is connected; its default is not used.")); }
			const FString Value = EW::Str(Args, TEXT("value"));
			const UEdGraphSchema_K2* K2 = Cast<UEdGraphSchema_K2>(G->GetSchema());
			if (P->PinType.PinCategory == UEdGraphSchema_K2::PC_Object || P->PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
			{
				FString OErr, Resolved;
				UObject* Obj = Value.IsEmpty() || Value == TEXT("None") ? nullptr : EW::ResolveObject(Value, false, Resolved, OErr);
				if (!Obj && !OErr.IsEmpty()) { return Bad(TEXT("INVALID_VALUE"), OErr); }
				K2->TrySetDefaultObject(*P, Obj);
			}
			else
			{
				const FString Problem = K2->IsPinDefaultValid(P, Value, nullptr, FText());
				if (!Problem.IsEmpty()) { return Bad(TEXT("INVALID_VALUE"), Problem); }
				K2->TrySetDefaultValue(*P, Value);
				if (P->DefaultValue != Value && !(P->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean && P->DefaultValue.Equals(Value, ESearchCase::IgnoreCase)))
				{
					return Bad(TEXT("READBACK_MISMATCH"), FString::Printf(TEXT("The pin reads back '%s', not '%s'."), *P->DefaultValue, *Value));
				}
			}
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("value"), P->DefaultObject ? P->DefaultObject->GetPathName() : P->DefaultValue);
			return Finish(BP, FEWResult::Ok(Data), /*bCompile*/ false);
		}, /*bTransactional*/ false);

	R.Register(TEXT("blueprint_remove_node"),
		TEXT("Remove a node from a Blueprint graph (its links are broken)."),
		TEXT(R"({"type":"object","properties":{"path":{"type":"string"},"graph":{"type":"string"},"node":{"type":"string"}},"required":["path","node"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UBlueprint* BP = LoadBP(Args, Err);
			if (!BP) { return Err; }
			TArray<FString> GraphNames;
			UEdGraph* G = FindGraph(BP, EW::Str(Args, TEXT("graph")), GraphNames);
			if (!G) { return FEWResult::Error(TEXT("UNKNOWN_GRAPH"), TEXT("No graph by that name."), EW::Suggest(EW::Str(Args, TEXT("graph")), GraphNames)); }
			UEdGraphNode* N = FindNode(G, EW::Str(Args, TEXT("node")));
			if (!N) { return Bad(TEXT("UNKNOWN_NODE"), TEXT("No node by that id.")); }
			if (!N->CanUserDeleteNode()) { return Bad(TEXT("NOT_DELETABLE"), TEXT("This node cannot be deleted.")); }
			FBlueprintEditorUtils::RemoveNode(BP, N, true);
			return Finish(BP, FEWResult::Ok(), /*bCompile*/ false);
		}, /*bTransactional*/ false);
}
