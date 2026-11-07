#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EWProps.h"

#include "AssetToolsModule.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "Engine/Texture.h"

namespace
{
	FEWResult Bad(const FString& Code, const FString& Msg, const TArray<FString>& Sug = {}) { return FEWResult::Error(Code, Msg, Sug); }

	struct FPropName { const TCHAR* Name; EMaterialProperty Prop; };
	const FPropName GProps[] = {
		{ TEXT("BaseColor"), MP_BaseColor }, { TEXT("Metallic"), MP_Metallic }, { TEXT("Specular"), MP_Specular },
		{ TEXT("Roughness"), MP_Roughness }, { TEXT("Anisotropy"), MP_Anisotropy }, { TEXT("EmissiveColor"), MP_EmissiveColor },
		{ TEXT("Opacity"), MP_Opacity }, { TEXT("OpacityMask"), MP_OpacityMask }, { TEXT("Normal"), MP_Normal },
		{ TEXT("Tangent"), MP_Tangent }, { TEXT("WorldPositionOffset"), MP_WorldPositionOffset }, { TEXT("SubsurfaceColor"), MP_SubsurfaceColor },
		{ TEXT("AmbientOcclusion"), MP_AmbientOcclusion }, { TEXT("Refraction"), MP_Refraction }, { TEXT("PixelDepthOffset"), MP_PixelDepthOffset },
	};

	bool FindProp(const FString& Name, EMaterialProperty& Out)
	{
		for (const FPropName& P : GProps) { if (Name.Equals(P.Name, ESearchCase::IgnoreCase)) { Out = P.Prop; return true; } }
		return false;
	}
	TArray<FString> PropNames() { TArray<FString> N; for (const FPropName& P : GProps) { N.Add(P.Name); } return N; }

	FString ParamName(const UMaterialExpression* E)
	{
		const FName N = E->GetParameterName();
		return N.IsNone() ? FString() : N.ToString();
	}

	UMaterialExpression* FindExpr(UMaterial* M, const FString& Id)
	{
		for (UMaterialExpression* E : UMaterialEditingLibrary::GetMaterialExpressions(M))
		{
			if (E && (E->GetName().Equals(Id, ESearchCase::IgnoreCase) || ParamName(E).Equals(Id, ESearchCase::IgnoreCase))) { return E; }
		}
		return nullptr;
	}

	TArray<FString> ExprIds(UMaterial* M)
	{
		TArray<FString> Out;
		for (UMaterialExpression* E : UMaterialEditingLibrary::GetMaterialExpressions(M)) { if (E) { Out.Add(E->GetName()); if (!ParamName(E).IsEmpty()) { Out.Add(ParamName(E)); } } }
		return Out;
	}

	/** Output pin by name, alias (#index, "" = first) . Returns the real name ("" for the default output). */
	bool ResolveOutput(UMaterialExpression* E, const FString& In, FString& Out, FString& OutError)
	{
		const TArray<FString> Names = UMaterialEditingLibrary::GetMaterialExpressionOutputNames(E);
		if (In.IsEmpty() || In == TEXT("#0") || In.Equals(TEXT("out"), ESearchCase::IgnoreCase) || In.Equals(TEXT("result"), ESearchCase::IgnoreCase)) { Out = Names.Num() ? Names[0] : FString(); return true; }
		if (In.StartsWith(TEXT("#")))
		{
			const int32 I = FCString::Atoi(*In.RightChop(1));
			if (Names.IsValidIndex(I)) { Out = Names[I]; return true; }
		}
		for (const FString& N : Names) { if (N.Equals(In, ESearchCase::IgnoreCase)) { Out = N; return true; } }
		OutError = FString::Printf(TEXT("%s has no output '%s'. Outputs: %s (or #index)."), *E->GetName(), *In, *FString::Join(Names, TEXT(", ")));
		return false;
	}

	/** Input pin by name, #index, or "" for single-input nodes (whose input name is often empty: A9). */
	bool ResolveInput(UMaterialExpression* E, const FString& In, FString& Out, FString& OutError)
	{
		const TArray<FString> Names = UMaterialEditingLibrary::GetMaterialExpressionInputNames(E);
		if (Names.Num() == 0) { OutError = FString::Printf(TEXT("%s has no inputs."), *E->GetName()); return false; }
		if (In.IsEmpty() && Names.Num() == 1) { Out = Names[0]; return true; }
		if (In.StartsWith(TEXT("#")))
		{
			const int32 I = FCString::Atoi(*In.RightChop(1));
			if (Names.IsValidIndex(I)) { Out = Names[I]; return true; }
		}
		for (const FString& N : Names) { if (N.Equals(In, ESearchCase::IgnoreCase)) { Out = N; return true; } }
		TArray<FString> Shown;
		for (int32 i = 0; i < Names.Num(); ++i) { Shown.Add(Names[i].IsEmpty() ? FString::Printf(TEXT("#%d"), i) : Names[i]); }
		OutError = FString::Printf(TEXT("%s has no input '%s'. Inputs: %s."), *E->GetName(), *In, *FString::Join(Shown, TEXT(", ")));
		return false;
	}

	TSharedRef<FJsonObject> MaterialJson(UMaterial* M)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("path"), M->GetPathName());
		TArray<TSharedPtr<FJsonValue>> Exprs;
		for (UMaterialExpression* E : UMaterialEditingLibrary::GetMaterialExpressions(M))
		{
			if (!E) { continue; }
			TSharedRef<FJsonObject> X = MakeShared<FJsonObject>();
			X->SetStringField(TEXT("id"), E->GetName());
			FString Cls = E->GetClass()->GetName();
			Cls.RemoveFromStart(TEXT("MaterialExpression"));
			X->SetStringField(TEXT("class"), Cls);
			if (!ParamName(E).IsEmpty()) { X->SetStringField(TEXT("parameter"), ParamName(E)); }
			const TArray<FString> InNames = UMaterialEditingLibrary::GetMaterialExpressionInputNames(E);
			const TArray<UMaterialExpression*> InNodes = UMaterialEditingLibrary::GetInputsForMaterialExpression(M, E);
			TArray<TSharedPtr<FJsonValue>> Inputs;
			for (int32 i = 0; i < InNames.Num(); ++i)
			{
				TSharedRef<FJsonObject> In = MakeShared<FJsonObject>();
				In->SetStringField(TEXT("name"), InNames[i].IsEmpty() ? FString::Printf(TEXT("#%d"), i) : InNames[i]);
				if (InNodes.IsValidIndex(i) && InNodes[i]) { In->SetStringField(TEXT("from"), InNodes[i]->GetName()); }
				Inputs.Add(MakeShared<FJsonValueObject>(In));
			}
			X->SetArrayField(TEXT("inputs"), Inputs);
			Exprs.Add(MakeShared<FJsonValueObject>(X));
		}
		J->SetArrayField(TEXT("expressions"), Exprs);
		TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
		for (const FPropName& P : GProps)
		{
			if (UMaterialExpression* E = UMaterialEditingLibrary::GetMaterialPropertyInputNode(M, P.Prop))
			{
				const FString OutName = UMaterialEditingLibrary::GetMaterialPropertyInputNodeOutputName(M, P.Prop);
				Props->SetStringField(P.Name, OutName.IsEmpty() ? E->GetName() : E->GetName() + TEXT(".") + OutName);
			}
		}
		J->SetObjectField(TEXT("properties"), Props);
		return J;
	}

	TSharedRef<FJsonObject> ParametersJson(UMaterialInterface* M, UMaterialInstanceConstant* MI)
	{
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		TArray<FName> Names;
		UMaterialEditingLibrary::GetScalarParameterNames(M, Names);
		for (const FName& N : Names)
		{
			float V = 0; M->GetScalarParameterValue(FHashedMaterialParameterInfo(N), V);
			TSharedRef<FJsonObject> X = MakeShared<FJsonObject>(); X->SetStringField(TEXT("type"), TEXT("scalar")); X->SetNumberField(TEXT("value"), V);
			if (MI) { X->SetBoolField(TEXT("overridden"), UMaterialEditingLibrary::IsMaterialInstanceParameterOverridden(MI, N)); }
			P->SetObjectField(N.ToString(), X);
		}
		Names.Reset();
		UMaterialEditingLibrary::GetVectorParameterNames(M, Names);
		for (const FName& N : Names)
		{
			FLinearColor V; M->GetVectorParameterValue(FHashedMaterialParameterInfo(N), V);
			TSharedRef<FJsonObject> X = MakeShared<FJsonObject>(); X->SetStringField(TEXT("type"), TEXT("vector"));
			X->SetArrayField(TEXT("value"), { MakeShared<FJsonValueNumber>(V.R), MakeShared<FJsonValueNumber>(V.G), MakeShared<FJsonValueNumber>(V.B), MakeShared<FJsonValueNumber>(V.A) });
			if (MI) { X->SetBoolField(TEXT("overridden"), UMaterialEditingLibrary::IsMaterialInstanceParameterOverridden(MI, N)); }
			P->SetObjectField(N.ToString(), X);
		}
		Names.Reset();
		UMaterialEditingLibrary::GetTextureParameterNames(M, Names);
		for (const FName& N : Names)
		{
			UTexture* V = nullptr; M->GetTextureParameterValue(FHashedMaterialParameterInfo(N), V);
			TSharedRef<FJsonObject> X = MakeShared<FJsonObject>(); X->SetStringField(TEXT("type"), TEXT("texture")); X->SetStringField(TEXT("value"), V ? V->GetPathName() : FString());
			if (MI) { X->SetBoolField(TEXT("overridden"), UMaterialEditingLibrary::IsMaterialInstanceParameterOverridden(MI, N)); }
			P->SetObjectField(N.ToString(), X);
		}
		Names.Reset();
		UMaterialEditingLibrary::GetStaticSwitchParameterNames(M, Names);
		for (const FName& N : Names)
		{
			bool V = false; FGuid G; M->GetStaticSwitchParameterDefaultValue(FHashedMaterialParameterInfo(N), V, G);
			TSharedRef<FJsonObject> X = MakeShared<FJsonObject>(); X->SetStringField(TEXT("type"), TEXT("switch")); X->SetBoolField(TEXT("value"), V);
			P->SetObjectField(N.ToString(), X);
		}
		return P;
	}

	bool SaveAsset(UObject* A, FEWResult& Res)
	{
		A->MarkPackageDirty();
		bool bDeferred = false;
		const bool bOk = EW::SaveOrDefer(A->GetOutermost(), bDeferred);
		if (!bOk) { Res.Warnings.Add(TEXT("Save failed; the change is in memory only.")); }
		return bOk;
	}

	bool CheckNew(const FString& Path, FString& Folder, FString& Name, FEWResult& OutErr)
	{
		FString Err;
		if (!EW::SplitAssetPath(Path, Folder, Name, Err)) { OutErr = Bad(TEXT("INVALID_PATH"), Err); return false; }
		if (EW::AssetPathTaken(Folder / Name)) { OutErr = Bad(TEXT("ASSET_EXISTS"), FString::Printf(TEXT("'%s' already exists."), *(Folder / Name))); return false; }
		return true;
	}

	FEWResult Recompile(UMaterial* M, FEWResult Res)
	{
		const TArray<FString> Errors = UMaterialEditingLibrary::RecompileMaterial(M);
		if (!Res.Data.IsValid()) { Res.Data = MakeShared<FJsonObject>(); }
		TArray<TSharedPtr<FJsonValue>> E;
		for (const FString& S : Errors) { E.Add(MakeShared<FJsonValueString>(S)); }
		Res.Data->SetArrayField(TEXT("compileErrors"), E);
		if (Errors.Num() && Res.bOk) { Res.Warnings.Add(TEXT("The material has compile errors (often an unconnected required input); see data.compileErrors.")); }
		Res.Data->SetBoolField(TEXT("saved"), SaveAsset(M, Res));
		return Res;
	}

	/** Validates every MI parameter value first (name, type), then writes; returns the read-back values. */
	FEWResult SetInstanceParams(UMaterialInstanceConstant* MI, const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid() || Params->Values.Num() == 0) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("parameters must be a non-empty object.")); }
		UMaterialInterface* Parent = MI->Parent;
		if (!Parent) { return Bad(TEXT("NO_PARENT"), TEXT("The instance has no parent material.")); }
		TArray<FName> Sc, Ve, Tx, Sw;
		UMaterialEditingLibrary::GetScalarParameterNames(Parent, Sc);
		UMaterialEditingLibrary::GetVectorParameterNames(Parent, Ve);
		UMaterialEditingLibrary::GetTextureParameterNames(Parent, Tx);
		UMaterialEditingLibrary::GetStaticSwitchParameterNames(Parent, Sw);
		TArray<FString> All;
		for (const TArray<FName>* L : { &Sc, &Ve, &Tx, &Sw }) { for (const FName& N : *L) { All.Add(N.ToString()); } }
		struct FStep { FName Name; int32 Kind; float S = 0; FLinearColor V; UTexture* T = nullptr; bool B = false; };
		TArray<FStep> Plan;
		for (const auto& KV : Params->Values)
		{
			const FString Key(*KV.Key);
			FStep Step; Step.Name = FName(*Key);
			auto Has = [&](const TArray<FName>& L) { return L.ContainsByPredicate([&](const FName& N) { return N.ToString().Equals(Key, ESearchCase::IgnoreCase); }); };
			auto Real = [&](const TArray<FName>& L) { return *L.FindByPredicate([&](const FName& N) { return N.ToString().Equals(Key, ESearchCase::IgnoreCase); }); };
			if (Has(Sc))
			{
				if (KV.Value->Type != EJson::Number) { return Bad(TEXT("INVALID_VALUE"), FString::Printf(TEXT("%s is a scalar; send a number. Nothing was written."), *Key)); }
				Step.Kind = 0; Step.Name = Real(Sc); Step.S = KV.Value->AsNumber();
			}
			else if (Has(Ve))
			{
				const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
				if (!KV.Value->TryGetArray(A) || A->Num() < 3 || A->Num() > 4) { return Bad(TEXT("INVALID_VALUE"), FString::Printf(TEXT("%s is a vector; send [r, g, b] or [r, g, b, a]. Nothing was written."), *Key)); }
				Step.Kind = 1; Step.Name = Real(Ve); Step.V = FLinearColor((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber(), A->Num() == 4 ? (*A)[3]->AsNumber() : 1.0);
			}
			else if (Has(Tx))
			{
				FString Err;
				Step.T = EW::LoadAs<UTexture>(KV.Value->AsString(), Err);
				if (!Step.T) { return Bad(TEXT("INVALID_VALUE"), FString::Printf(TEXT("%s: %s Nothing was written."), *Key, *Err)); }
				Step.Kind = 2; Step.Name = Real(Tx);
			}
			else if (Has(Sw))
			{
				if (KV.Value->Type != EJson::Boolean) { return Bad(TEXT("INVALID_VALUE"), FString::Printf(TEXT("%s is a static switch; send true or false. Nothing was written."), *Key)); }
				Step.Kind = 3; Step.Name = Real(Sw); Step.B = KV.Value->AsBool();
			}
			else
			{
				return Bad(TEXT("UNKNOWN_PARAMETER"), FString::Printf(TEXT("The parent has no parameter '%s'. Nothing was written."), *Key), EW::Suggest(Key, All));
			}
			Plan.Add(Step);
		}
		MI->Modify();
		TSharedRef<FJsonObject> Back = MakeShared<FJsonObject>();
		for (const FStep& S : Plan)
		{
			switch (S.Kind)
			{
			case 0: UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MI, S.Name, S.S);
				Back->SetNumberField(S.Name.ToString(), UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(MI, S.Name)); break;
			case 1:
			{
				UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(MI, S.Name, S.V);
				const FLinearColor C = UMaterialEditingLibrary::GetMaterialInstanceVectorParameterValue(MI, S.Name);
				Back->SetArrayField(S.Name.ToString(), { MakeShared<FJsonValueNumber>(C.R), MakeShared<FJsonValueNumber>(C.G), MakeShared<FJsonValueNumber>(C.B), MakeShared<FJsonValueNumber>(C.A) });
				if (!C.Equals(S.V, 1e-4f)) { return Bad(TEXT("READBACK_MISMATCH"), FString::Printf(TEXT("%s reads back differently."), *S.Name.ToString())); }
				break;
			}
			case 2: UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MI, S.Name, S.T);
				Back->SetStringField(S.Name.ToString(), GetPathNameSafe(UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(MI, S.Name))); break;
			case 3: UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MI, S.Name, S.B);
				Back->SetBoolField(S.Name.ToString(), UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(MI, S.Name)); break;
			}
			if (S.Kind == 0 && !FMath::IsNearlyEqual(UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(MI, S.Name), S.S, 1e-4f))
			{
				return Bad(TEXT("READBACK_MISMATCH"), FString::Printf(TEXT("%s reads back differently."), *S.Name.ToString()));
			}
		}
		UMaterialEditingLibrary::UpdateMaterialInstance(MI);
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetObjectField(TEXT("values"), Back);
		FEWResult Res = FEWResult::Ok(Data);
		Res.Data->SetBoolField(TEXT("saved"), SaveAsset(MI, Res));
		return Res;
	}
}

void RegisterMaterialTools(FEWRegistry& R)
{
	R.Register(TEXT("material_create"),
		TEXT("Create a material. Optionally wire parameters straight to material inputs: inputs {BaseColor: {parameter: 'Tint', value: [1,0,0]}, Roughness: {parameter: 'Rough', value: 0.5}, Metallic: {value: 1}} (no parameter = constant)."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string","description":"e.g. /Game/Materials/M_Tint"},
			"inputs":{"type":"object","description":"Material input -> {parameter?, value} with a number (scalar) or [r,g,b(,a)] (vector)."}},
			"required":["path"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Folder, Name;
			FEWResult Err;
			if (!CheckNew(EW::Str(Args, TEXT("path")), Folder, Name, Err)) { return Err; }
			// Check the inputs first.
			struct FIn { EMaterialProperty Prop; FString Param; TArray<double> V; };
			TArray<FIn> Plan;
			const TSharedPtr<FJsonObject>* Inputs = nullptr;
			if (Args->TryGetObjectField(TEXT("inputs"), Inputs))
			{
				for (const auto& KV : (*Inputs)->Values)
				{
					FIn In;
					if (!FindProp(FString(*KV.Key), In.Prop)) { return Bad(TEXT("UNKNOWN_INPUT"), FString::Printf(TEXT("No material input '%s'."), *FString(*KV.Key)), EW::Suggest(FString(*KV.Key), PropNames())); }
					const TSharedPtr<FJsonObject>* O = nullptr;
					if (!KV.Value->TryGetObject(O)) { return Bad(TEXT("INVALID_VALUE"), FString::Printf(TEXT("%s must be {parameter?, value}."), *FString(*KV.Key))); }
					for (const auto& K2 : (*O)->Values) { if (FString(*K2.Key) != TEXT("parameter") && FString(*K2.Key) != TEXT("value")) { return Bad(TEXT("UNKNOWN_ARGUMENT"), FString::Printf(TEXT("%s has unknown key '%s' (parameter, value)."), *FString(*KV.Key), *FString(*K2.Key))); } }
					(*O)->TryGetStringField(TEXT("parameter"), In.Param);
					const TSharedPtr<FJsonValue> V = (*O)->TryGetField(TEXT("value"));
					if (!V.IsValid()) { return Bad(TEXT("INVALID_VALUE"), FString::Printf(TEXT("%s needs value."), *FString(*KV.Key))); }
					if (V->Type == EJson::Number) { In.V.Add(V->AsNumber()); }
					else if (V->Type == EJson::Array) { for (const auto& X : V->AsArray()) { In.V.Add(X->AsNumber()); } if (In.V.Num() < 3 || In.V.Num() > 4) { return Bad(TEXT("INVALID_VALUE"), TEXT("A vector value is [r,g,b] or [r,g,b,a].")); } }
					else { return Bad(TEXT("INVALID_VALUE"), FString::Printf(TEXT("%s value must be a number or [r,g,b(,a)]."), *FString(*KV.Key))); }
					Plan.Add(In);
				}
			}
			UMaterial* M = Cast<UMaterial>(FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().CreateAsset(Name, Folder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
			if (!M) { return Bad(TEXT("CREATE_FAILED"), TEXT("The editor could not create the material.")); }
			int32 Y = 0;
			for (const FIn& In : Plan)
			{
				const bool bVec = In.V.Num() > 1;
				UClass* C = nullptr;
				if (In.Param.IsEmpty()) { C = FindObject<UClass>(nullptr, bVec ? TEXT("/Script/Engine.MaterialExpressionConstant4Vector") : TEXT("/Script/Engine.MaterialExpressionConstant")); }
				else { C = FindObject<UClass>(nullptr, bVec ? TEXT("/Script/Engine.MaterialExpressionVectorParameter") : TEXT("/Script/Engine.MaterialExpressionScalarParameter")); }
				UMaterialExpression* E = UMaterialEditingLibrary::CreateMaterialExpression(M, C, -400, Y);
				Y += 160;
				TSharedPtr<FJsonObject> Vals = MakeShared<FJsonObject>();
				if (!In.Param.IsEmpty()) { Vals->SetStringField(TEXT("ParameterName"), In.Param); }
				if (bVec)
				{
					TSharedPtr<FJsonObject> Col = MakeShared<FJsonObject>();
					Col->SetNumberField(TEXT("R"), In.V[0]); Col->SetNumberField(TEXT("G"), In.V[1]); Col->SetNumberField(TEXT("B"), In.V[2]); Col->SetNumberField(TEXT("A"), In.V.Num() == 4 ? In.V[3] : 1.0);
					Vals->SetObjectField(In.Param.IsEmpty() ? TEXT("Constant") : TEXT("DefaultValue"), Col);
				}
				else { Vals->SetNumberField(In.Param.IsEmpty() ? TEXT("R") : TEXT("DefaultValue"), In.V[0]); }
				FEWResult W = EW::WriteValues(E->GetClass(), E, E, Vals);
				if (!W.bOk) { return W; }
				UMaterialEditingLibrary::ConnectMaterialProperty(E, FString(), In.Prop);
			}
			FEWResult Res = FEWResult::Ok(MaterialJson(M));
			Res = Recompile(M, Res);
			Res.Data->SetObjectField(TEXT("parameters"), ParametersJson(M, nullptr));
			return Res;
		}, /*bTransactional*/ false);

	R.Register(TEXT("material_info"),
		TEXT("Describe a material or material instance: expressions with their inputs and links, which node feeds each material input, and parameters with values (instances: overridden flags)."),
		TEXT(R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Err;
			UMaterialInterface* MI = EW::LoadAs<UMaterialInterface>(EW::Str(Args, TEXT("path")), Err);
			if (!MI) { return Bad(TEXT("MATERIAL_NOT_FOUND"), Err); }
			if (UMaterial* M = Cast<UMaterial>(MI))
			{
				TSharedRef<FJsonObject> J = MaterialJson(M);
				J->SetObjectField(TEXT("parameters"), ParametersJson(M, nullptr));
				return FEWResult::Ok(J);
			}
			UMaterialInstanceConstant* C = Cast<UMaterialInstanceConstant>(MI);
			TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetStringField(TEXT("path"), MI->GetPathName());
			J->SetStringField(TEXT("parent"), C && C->Parent ? C->Parent->GetPathName() : FString());
			J->SetObjectField(TEXT("parameters"), ParametersJson(MI, C));
			return FEWResult::Ok(J);
		});

	R.Register(TEXT("material_add_expression"),
		TEXT("Add an expression node to a material, e.g. Multiply, Add, Lerp, Constant3Vector, TextureCoordinate, ScalarParameter, VectorParameter, TextureSampleParameter2D, StaticSwitchParameter. name sets the parameter name; values sets node properties (checked like object_set). Returns the node id for material_connect."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string"},
			"class":{"type":"string","description":"Expression class without the MaterialExpression prefix."},
			"name":{"type":"string","description":"Parameter name (parameter nodes only)."},
			"values":{"type":"object"},
			"position":{"type":"array","items":{"type":"number"}}},
			"required":["path","class"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Err;
			UMaterial* M = EW::LoadAs<UMaterial>(EW::Str(Args, TEXT("path")), Err);
			if (!M) { return Bad(TEXT("MATERIAL_NOT_FOUND"), Err); }
			FString Cls = EW::Str(Args, TEXT("class"));
			Cls.RemoveFromStart(TEXT("MaterialExpression"));
			UClass* C = FindObject<UClass>(nullptr, *(TEXT("/Script/Engine.MaterialExpression") + Cls));
			if (!C)
			{
				TArray<FString> Names;
				for (TObjectIterator<UClass> It; It; ++It)
				{
					if (It->IsChildOf(UMaterialExpression::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract)) { FString N = It->GetName(); N.RemoveFromStart(TEXT("MaterialExpression")); Names.Add(N); }
				}
				for (const FString& N : Names) { if (N.Equals(Cls, ESearchCase::IgnoreCase)) { C = FindFirstObject<UClass>(*(TEXT("MaterialExpression") + N), EFindFirstObjectOptions::None); break; } }
				if (!C) { return Bad(TEXT("UNKNOWN_CLASS"), FString::Printf(TEXT("No material expression '%s'."), *Cls), EW::Suggest(Cls, Names)); }
			}
			if (C->HasAnyClassFlags(CLASS_Abstract) || !C->IsChildOf(UMaterialExpression::StaticClass())) { return Bad(TEXT("INVALID_CLASS"), TEXT("Not a concrete material expression class.")); }
			const FString Param = EW::Str(Args, TEXT("name"));
			const bool bIsParam = C->IsChildOf(UMaterialExpressionParameter::StaticClass()) || C->IsChildOf(UMaterialExpressionTextureSampleParameter::StaticClass());
			if (!Param.IsEmpty() && !bIsParam) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("name is only for parameter nodes.")); }
			if (!Param.IsEmpty() && FindExpr(M, Param)) { return Bad(TEXT("NAME_IN_USE"), FString::Printf(TEXT("The material already has '%s'."), *Param)); }
			int32 X = -400, Y = 0;
			const TArray<TSharedPtr<FJsonValue>>* Pos = nullptr;
			if (Args->TryGetArrayField(TEXT("position"), Pos) && Pos->Num() == 2) { X = (*Pos)[0]->AsNumber(); Y = (*Pos)[1]->AsNumber(); }
			// Validate values on a scratch node first so a bad value leaves the material untouched.
			const TSharedPtr<FJsonObject>* Values = nullptr;
			Args->TryGetObjectField(TEXT("values"), Values);
			if (Values && (*Values)->Values.Num())
			{
				UMaterialExpression* Probe = NewObject<UMaterialExpression>(GetTransientPackage(), C);
				FEWResult W = EW::WriteValues(C, Probe, nullptr, *Values);
				if (!W.bOk) { return W; }
			}
			UMaterialExpression* E = UMaterialEditingLibrary::CreateMaterialExpression(M, C, X, Y);
			if (!E) { return Bad(TEXT("CREATE_FAILED"), TEXT("The expression could not be created.")); }
			if (!Param.IsEmpty()) { E->SetParameterName(FName(*Param)); }
			if (Values && (*Values)->Values.Num()) { EW::WriteValues(C, E, E, *Values); }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("id"), E->GetName());
			TArray<TSharedPtr<FJsonValue>> Ins, Outs;
			for (const FString& N : UMaterialEditingLibrary::GetMaterialExpressionInputNames(E)) { Ins.Add(MakeShared<FJsonValueString>(N)); }
			for (const FString& N : UMaterialEditingLibrary::GetMaterialExpressionOutputNames(E)) { Outs.Add(MakeShared<FJsonValueString>(N)); }
			Data->SetArrayField(TEXT("inputs"), Ins);
			Data->SetArrayField(TEXT("outputs"), Outs);
			return Recompile(M, FEWResult::Ok(Data));
		}, /*bTransactional*/ false);

	R.Register(TEXT("material_connect"),
		TEXT("Connect a material node output to another node's input or to a material input (BaseColor, Roughness, Normal...). Pins by name, #index, or empty for single-input nodes. The link is read back and the material recompiled."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string"},
			"from":{"type":"string","description":"Node id or parameter name."},
			"fromOutput":{"type":"string","description":"Output name or #index; default first output."},
			"to":{"type":"string","description":"Node id, parameter name, or a material input such as BaseColor."},
			"toInput":{"type":"string","description":"Input name or #index (node targets only)."}},
			"required":["path","from","to"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Err;
			UMaterial* M = EW::LoadAs<UMaterial>(EW::Str(Args, TEXT("path")), Err);
			if (!M) { return Bad(TEXT("MATERIAL_NOT_FOUND"), Err); }
			UMaterialExpression* From = FindExpr(M, EW::Str(Args, TEXT("from")));
			if (!From) { return Bad(TEXT("UNKNOWN_NODE"), FString::Printf(TEXT("No node '%s'."), *EW::Str(Args, TEXT("from"))), EW::Suggest(EW::Str(Args, TEXT("from")), ExprIds(M))); }
			FString OutName;
			if (!ResolveOutput(From, EW::Str(Args, TEXT("fromOutput")), OutName, Err)) { return Bad(TEXT("UNKNOWN_PIN"), Err); }
			const FString To = EW::Str(Args, TEXT("to"));
			EMaterialProperty Prop;
			if (FindProp(To, Prop))
			{
				if (!UMaterialEditingLibrary::ConnectMaterialProperty(From, OutName, Prop)) { return Bad(TEXT("CONNECTION_REFUSED"), TEXT("The material input refused the connection.")); }
				if (UMaterialEditingLibrary::GetMaterialPropertyInputNode(M, Prop) != From) { return Bad(TEXT("READBACK_MISMATCH"), TEXT("The material input is not connected after the call.")); }
			}
			else
			{
				UMaterialExpression* ToE = FindExpr(M, To);
				if (!ToE)
				{
					TArray<FString> Cand = ExprIds(M);
					Cand.Append(PropNames());
					return Bad(TEXT("UNKNOWN_NODE"), FString::Printf(TEXT("No node or material input '%s'."), *To), EW::Suggest(To, Cand));
				}
				FString InName;
				if (!ResolveInput(ToE, EW::Str(Args, TEXT("toInput")), InName, Err)) { return Bad(TEXT("UNKNOWN_PIN"), Err); }
				if (!UMaterialEditingLibrary::ConnectMaterialExpressions(From, OutName, ToE, InName)) { return Bad(TEXT("CONNECTION_REFUSED"), TEXT("The connection was refused.")); }
				const int32 Index = UMaterialEditingLibrary::GetMaterialExpressionInputNames(ToE).IndexOfByKey(InName);
				const TArray<UMaterialExpression*> Ins = UMaterialEditingLibrary::GetInputsForMaterialExpression(M, ToE);
				if (!Ins.IsValidIndex(Index) || Ins[Index] != From) { return Bad(TEXT("READBACK_MISMATCH"), TEXT("The input is not connected after the call.")); }
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("from"), From->GetName() + (OutName.IsEmpty() ? FString() : TEXT(".") + OutName));
			Data->SetStringField(TEXT("to"), To);
			return Recompile(M, FEWResult::Ok(Data));
		}, /*bTransactional*/ false);

	R.Register(TEXT("material_instance_create"),
		TEXT("Create a material instance of a material (or of another instance), with parameter values checked against the parent's parameters: numbers for scalars, [r,g,b(,a)] for vectors, texture paths, true/false for static switches."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string"},"parent":{"type":"string"},
			"parameters":{"type":"object"}},
			"required":["path","parent"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Folder, Name, Err;
			FEWResult E;
			if (!CheckNew(EW::Str(Args, TEXT("path")), Folder, Name, E)) { return E; }
			UMaterialInterface* Parent = EW::LoadAs<UMaterialInterface>(EW::Str(Args, TEXT("parent")), Err);
			if (!Parent) { return Bad(TEXT("MATERIAL_NOT_FOUND"), Err); }
			const TSharedPtr<FJsonObject>* Params = nullptr;
			const bool bParams = Args->TryGetObjectField(TEXT("parameters"), Params) && (*Params)->Values.Num();
			if (bParams)
			{
				// Dry-run the parameters on a transient instance first: a bad name creates nothing.
				UMaterialInstanceConstant* Probe = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
				Probe->SetParentEditorOnly(Parent);
				TArray<FName> Sc, Ve, Tx, Sw;
				UMaterialEditingLibrary::GetScalarParameterNames(Parent, Sc); UMaterialEditingLibrary::GetVectorParameterNames(Parent, Ve);
				UMaterialEditingLibrary::GetTextureParameterNames(Parent, Tx); UMaterialEditingLibrary::GetStaticSwitchParameterNames(Parent, Sw);
				TArray<FString> All;
				for (const TArray<FName>* L : { &Sc, &Ve, &Tx, &Sw }) { for (const FName& N : *L) { All.Add(N.ToString()); } }
				for (const auto& KV : (*Params)->Values)
				{
					if (!All.ContainsByPredicate([&](const FString& N) { return N.Equals(FString(*KV.Key), ESearchCase::IgnoreCase); }))
					{
						return Bad(TEXT("UNKNOWN_PARAMETER"), FString::Printf(TEXT("The parent has no parameter '%s'. Nothing was created."), *FString(*KV.Key)), EW::Suggest(FString(*KV.Key), All));
					}
				}
			}
			UMaterialInstanceConstantFactoryNew* F = NewObject<UMaterialInstanceConstantFactoryNew>();
			F->InitialParent = Parent;
			UMaterialInstanceConstant* MI = Cast<UMaterialInstanceConstant>(FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().CreateAsset(Name, Folder, UMaterialInstanceConstant::StaticClass(), F));
			if (!MI) { return Bad(TEXT("CREATE_FAILED"), TEXT("The editor could not create the instance.")); }
			FEWResult Res = bParams ? SetInstanceParams(MI, *Params) : FEWResult::Ok();
			if (!Res.bOk) { Res.Message += TEXT(" The instance was created without these values."); return Res; }
			Res.Data->SetStringField(TEXT("path"), MI->GetPathName());
			Res.Data->SetStringField(TEXT("parent"), Parent->GetPathName());
			if (!bParams) { Res.Data->SetBoolField(TEXT("saved"), SaveAsset(MI, Res)); }
			return Res;
		}, /*bTransactional*/ false);

	R.Register(TEXT("material_instance_set"),
		TEXT("Set parameter values on a material instance. Every name and value is checked against the parent first (unknown name = error with suggestions, nothing written); values are read back and saved."),
		TEXT(R"({"type":"object","properties":{"path":{"type":"string"},"parameters":{"type":"object"}},"required":["path","parameters"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Err;
			UMaterialInstanceConstant* MI = EW::LoadAs<UMaterialInstanceConstant>(EW::Str(Args, TEXT("path")), Err);
			if (!MI) { return Bad(TEXT("MATERIAL_NOT_FOUND"), Err); }
			const TSharedPtr<FJsonObject>* Params = nullptr;
			Args->TryGetObjectField(TEXT("parameters"), Params);
			return SetInstanceParams(MI, Params ? *Params : nullptr);
		}, /*bTransactional*/ false);
}
