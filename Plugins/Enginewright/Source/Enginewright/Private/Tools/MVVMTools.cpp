// MVVM (UMG Viewmodel): list, add, bind, unbind and remove view models and bindings of a Widget Blueprint.
// This file only holds the tools' schemas, the undo step, compile and save. The code that touches the MVVM plugin
// lives in the separate EnginewrightMVVM plugin (see EnginewrightMVVMBackend.h), so Enginewright loads and works
// in a project that does not use MVVM, and it never turns the MVVM plugin on by itself.
#include "EnginewrightTool.h"
#include "EnginewrightMVVMBackend.h"
#include "EWCommon.h"
#include "EWProps.h"

#include "Editor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "WidgetBlueprint.h"

namespace
{
	TMap<FString, FEWMVVMBackend::FOp>& Ops()
	{
		static TMap<FString, FEWMVVMBackend::FOp> Map;
		return Map;
	}

	FEWResult NotEnabled()
	{
		return FEWResult::Error(TEXT("MVVM_NOT_ENABLED"),
			TEXT("MVVM support is off in this project. Enable the plugins 'ModelViewViewModel' (UMG Viewmodel, by Epic) and 'EnginewrightMVVM' "
				"(Edit > Plugins, search 'viewmodel', or add both to the Plugins list of the .uproject), then restart the editor. "
				"Enginewright does not turn them on for you."));
	}

	UWidgetBlueprint* LoadWidget(const TSharedPtr<FJsonObject>& Args, FEWResult& OutErr)
	{
		FString Err;
		UWidgetBlueprint* WB = EW::LoadAs<UWidgetBlueprint>(EW::Str(Args, TEXT("widget")), Err);
		if (!WB) { OutErr = FEWResult::Error(TEXT("ASSET_NOT_FOUND"), Err); }
		return WB;
	}

	/** Compiles, collects the errors (compiler log and MVVM binding messages) and saves. */
	FEWResult CompileAndSave(UWidgetBlueprint* WB, FEWResult Res)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WB);
		FCompilerResultsLog Log;
		Log.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(WB, EBlueprintCompileOptions::SkipGarbageCollection, &Log);
		if (!Res.Data.IsValid()) { Res.Data = MakeShared<FJsonObject>(); }

		TArray<TSharedPtr<FJsonValue>> Errors;
		for (const TSharedRef<FTokenizedMessage>& M : Log.Messages)
		{
			if (M->GetSeverity() == EMessageSeverity::Error) { Errors.Add(MakeShared<FJsonValueString>(M->ToText().ToString())); }
		}

		// Read back: what the view model now holds, including the binding messages the compile produced.
		if (const FEWMVVMBackend::FOp* List = FEWMVVMBackend::Find(TEXT("list")))
		{
			FEWResult Back = (*List)(WB, MakeShared<FJsonObject>());
			if (Back.bOk && Back.Data.IsValid())
			{
				Res.Data->SetObjectField(TEXT("readBack"), Back.Data);
				const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
				if (Back.Data->TryGetArrayField(TEXT("bindings"), Bindings))
				{
					for (const TSharedPtr<FJsonValue>& B : *Bindings)
					{
						const TArray<TSharedPtr<FJsonValue>>* BindingErrors = nullptr;
						if (B->AsObject()->TryGetArrayField(TEXT("errors"), BindingErrors)) { Errors.Append(*BindingErrors); }
					}
				}
			}
		}

		Res.Data->SetBoolField(TEXT("compiled"), WB->Status != BS_Error && Errors.Num() == 0);
		if (Errors.Num() || WB->Status == BS_Error)
		{
			Res.bOk = false;
			Res.ErrorCode = TEXT("COMPILE_ERROR");
			Res.Message = TEXT("The change was applied but the Widget Blueprint does not compile; see data.compileErrors. Undo it (Ctrl+Z) or fix the binding with viewmodel_unbind.");
			Res.Data->SetArrayField(TEXT("compileErrors"), Errors);
		}
		bool bDeferred = false;
		const bool bSaved = EW::SaveOrDefer(WB->GetOutermost(), bDeferred);
		Res.Data->SetBoolField(TEXT("saved"), bSaved && !bDeferred);
		if (!bSaved) { Res.Warnings.Add(TEXT("Saving the Widget Blueprint failed.")); }
		return Res;
	}

	FEWResult RunRead(const FString& Op, const TSharedPtr<FJsonObject>& Args)
	{
		const FEWMVVMBackend::FOp* Fn = FEWMVVMBackend::Find(Op);
		if (!Fn) { return NotEnabled(); }
		FEWResult Err;
		UWidgetBlueprint* WB = LoadWidget(Args, Err);
		if (!WB) { return Err; }
		return (*Fn)(WB, Args);
	}

	FEWResult RunWrite(const FString& Op, const TSharedPtr<FJsonObject>& Args)
	{
		const FEWMVVMBackend::FOp* Fn = FEWMVVMBackend::Find(Op);
		if (!Fn) { return NotEnabled(); }
		FEWResult Err;
		UWidgetBlueprint* WB = LoadWidget(Args, Err);
		if (!WB) { return Err; }

		// One undo step for the whole call. The operation validates everything before it writes.
		FScopedTransaction Tx(FText::FromString(FString::Printf(TEXT("Enginewright: viewmodel_%s"), *Op)));
		WB->Modify();
		FEWResult Res = (*Fn)(WB, Args);
		if (!Res.bOk)
		{
			EW::CancelUnlessBatch(Tx);
			return Res;
		}
		bool bChanged = true;
		if (Res.Data.IsValid()) { Res.Data->TryGetBoolField(TEXT("changed"), bChanged); }
		if (!bChanged)
		{
			EW::CancelUnlessBatch(Tx);
			return Res;
		}
		return CompileAndSave(WB, MoveTemp(Res));
	}
}

void FEWMVVMBackend::Set(const FString& Op, FOp Fn) { Ops().Add(Op, MoveTemp(Fn)); }
void FEWMVVMBackend::Clear() { Ops().Empty(); }
const FEWMVVMBackend::FOp* FEWMVVMBackend::Find(const FString& Op) { return Ops().Find(Op); }
bool FEWMVVMBackend::IsAvailable() { return Ops().Num() > 0; }

void RegisterMVVMTools(FEWRegistry& R)
{
	R.Register(TEXT("viewmodel_list"),
		TEXT("List the MVVM view models of a Widget Blueprint (name, class, creation type) and its view bindings (source -> target, mode, conversion function, enabled, compile errors). Read only. "
			"Needs the ModelViewViewModel and EnginewrightMVVM plugins; otherwise it returns MVVM_NOT_ENABLED and says how to turn them on. Use viewmodel_add / viewmodel_bind to change them."),
		TEXT(R"({"type":"object","properties":{
			"widget":{"type":"string","description":"Widget Blueprint path, e.g. /Game/UI/WBP_Shop."}},
			"required":["widget"]})"),
		/*bWrites*/ false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult { return RunRead(TEXT("list"), Args); });

	R.Register(TEXT("viewmodel_add"),
		TEXT("Add a view model (a class derived from MVVMViewModelBase, C++ or Blueprint) to a Widget Blueprint, like the Viewmodels panel does. "
			"Name defaults to the class name. creationType defaults to CreateInstance (the widget creates the instance); use Manual when the game sets it. "
			"Adding the same class under the same name again changes nothing. Compiles and saves the Widget Blueprint; one undo step. "
			"Needs the ModelViewViewModel and EnginewrightMVVM plugins (else MVVM_NOT_ENABLED). Bind its properties with viewmodel_bind."),
		TEXT(R"({"type":"object","properties":{
			"widget":{"type":"string","description":"Widget Blueprint path, e.g. /Game/UI/WBP_Shop."},
			"viewModel":{"type":"string","description":"View model class: /Script/MyGame.ShopViewModel, a short C++ class name or a Blueprint class path."},
			"name":{"type":"string","description":"Name of the view model in the widget; default: the class name."},
			"creationType":{"type":"string","enum":["CreateInstance","Manual","GlobalViewModelCollection","PropertyPath"],"description":"How the widget gets its instance; default CreateInstance."},
			"globalIdentifier":{"type":"string","description":"Needed for GlobalViewModelCollection: the identifier of the registered view model."},
			"propertyPath":{"type":"string","description":"Needed for PropertyPath: the path that returns the view model instance."}},
			"required":["widget","viewModel"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult { return RunWrite(TEXT("add"), Args); });

	R.Register(TEXT("viewmodel_bind"),
		TEXT("Bind a view model property (or function) to a widget property, like the View Bindings panel: source = a view model property (FieldNotify) or function, target = widget name and property, e.g. Gold.Text. "
			"mode defaults to OneWayToDestination. If the types differ (an int into a Text), name a conversion function (Conv_IntToText, or /Script/Engine.KismetTextLibrary:Conv_IntToText); "
			"without one the call fails with TYPE_MISMATCH and lists the conversion functions that fit. The same binding twice changes nothing. "
			"Validates everything first, compiles, saves, reads the bindings back and fails with COMPILE_ERROR if the binding does not compile. One undo step. "
			"Needs the ModelViewViewModel and EnginewrightMVVM plugins (else MVVM_NOT_ENABLED). Remove with viewmodel_unbind."),
		TEXT(R"({"type":"object","properties":{
			"widget":{"type":"string","description":"Widget Blueprint path, e.g. /Game/UI/WBP_Shop."},
			"viewModel":{"type":"string","description":"Name of the view model in the widget (see viewmodel_list), or its class."},
			"source":{"type":"string","description":"View model property or function, e.g. Gold (a dotted path like Stats.Gold goes through struct or object properties)."},
			"target":{"type":"string","description":"Widget and property, e.g. Gold.Text."},
			"mode":{"type":"string","enum":["OneTimeToDestination","OneWayToDestination","TwoWay","OneWayToSource"],"description":"Default OneWayToDestination."},
			"conversion":{"type":"string","description":"Conversion function, e.g. Conv_IntToText or /Script/Engine.KismetTextLibrary:Conv_IntToText."}},
			"required":["widget","viewModel","source","target"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult { return RunWrite(TEXT("bind"), Args); });

	R.Register(TEXT("viewmodel_unbind"),
		TEXT("Remove view bindings from a Widget Blueprint: every binding whose target is the given widget property (e.g. Gold.Text), optionally only those from one view model or source. "
			"Fails with BINDING_NOT_FOUND when none matches. Compiles and saves; one undo step. Needs the ModelViewViewModel and EnginewrightMVVM plugins (else MVVM_NOT_ENABLED)."),
		TEXT(R"({"type":"object","properties":{
			"widget":{"type":"string","description":"Widget Blueprint path, e.g. /Game/UI/WBP_Shop."},
			"target":{"type":"string","description":"Widget and property, e.g. Gold.Text."},
			"viewModel":{"type":"string","description":"Only bindings from this view model (name or class)."},
			"source":{"type":"string","description":"Only bindings from this view model property or function."}},
			"required":["widget","target"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult { return RunWrite(TEXT("unbind"), Args); });

	R.Register(TEXT("viewmodel_remove"),
		TEXT("Remove a view model from a Widget Blueprint. Fails with VIEWMODEL_IN_USE while bindings still use it, unless removeBindings is true (then those bindings go too). "
			"Compiles and saves; one undo step. Needs the ModelViewViewModel and EnginewrightMVVM plugins (else MVVM_NOT_ENABLED)."),
		TEXT(R"({"type":"object","properties":{
			"widget":{"type":"string","description":"Widget Blueprint path, e.g. /Game/UI/WBP_Shop."},
			"viewModel":{"type":"string","description":"Name of the view model in the widget (see viewmodel_list), or its class."},
			"removeBindings":{"type":"boolean","description":"Also remove the bindings that use it; default false."}},
			"required":["widget","viewModel"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult { return RunWrite(TEXT("remove"), Args); });
}
