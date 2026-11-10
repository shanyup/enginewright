// EnginewrightMVVM: the part of the viewmodel_* tools that touches Epic's MVVM (UMG Viewmodel) plugin.
// It registers its operations with the Enginewright module (FEWMVVMBackend); Enginewright wraps them with the
// tool schema, one undo step, compile and save.
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "EnginewrightTool.h"
#include "EnginewrightMVVMBackend.h"

#include "Bindings/MVVMBindingHelper.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Editor.h"
#include "INotifyFieldValueChanged.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "MVVMBlueprintFunctionReference.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"
#include "MVVMBlueprintViewConversionFunction.h"
#include "MVVMBlueprintViewModelContext.h"
#include "MVVMEditorSubsystem.h"
#include "MVVMPropertyPath.h"
#include "Types/MVVMBindingMode.h"
#include "UObject/UObjectIterator.h"
#include "WidgetBlueprint.h"

using UE::MVVM::FMVVMConstFieldVariant;

namespace
{
	FEWResult Bad(const FString& Code, const FString& Msg, const TArray<FString>& Sug = {}) { return FEWResult::Error(Code, Msg, Sug); }

	TSharedPtr<FJsonValue> Str(const FString& S) { return MakeShared<FJsonValueString>(S); }

	UMVVMEditorSubsystem* Subsystem() { return GEditor ? GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>() : nullptr; }

	/** Up to Max names that contain Wanted (case-insensitive), else the first Max names. */
	TArray<FString> Near(const FString& Wanted, const TArray<FString>& Names, int32 Max = 6)
	{
		TArray<FString> Out;
		for (const FString& N : Names) { if (!Wanted.IsEmpty() && N.Contains(Wanted, ESearchCase::IgnoreCase)) { Out.AddUnique(N); } }
		for (const FString& N : Names) { if (Out.Num() >= Max) { break; } Out.AddUnique(N); }
		if (Out.Num() > Max) { Out.SetNum(Max); }
		return Out;
	}

	FString Opt(const TSharedPtr<FJsonObject>& Args, const TCHAR* Key)
	{
		FString V;
		return Args.IsValid() && Args->TryGetStringField(Key, V) ? V.TrimStartAndEnd() : FString();
	}

	FString ClassPath(const UClass* C) { return C ? C->GetPathName() : FString(); }

	/** Property path text without the view model or widget: "Gold" or "Stats.Gold". */
	FString FieldText(const FMVVMBlueprintPropertyPath& P)
	{
		TArray<FString> Parts;
		for (const FMVVMBlueprintFieldPath& F : P.GetFieldPaths()) { Parts.Add(F.GetRawFieldName().ToString()); }
		return FString::Join(Parts, TEXT("."));
	}

	/**
	 * The source of a binding. With a conversion function the binding's own SourcePath is empty: the source is the path
	 * set on the argument pin of the conversion function.
	 */
	FMVVMBlueprintPropertyPath EffectiveSource(UWidgetBlueprint* WB, const FMVVMBlueprintViewBinding& B)
	{
		if (B.SourcePath.IsValid()) { return B.SourcePath; }
		UMVVMEditorSubsystem* Sub = Subsystem();
		UMVVMBlueprintViewConversionFunction* Conv = B.Conversion.SourceToDestinationConversion;
		if (!Sub || !Conv) { return FMVVMBlueprintPropertyPath(); }
		if (const UFunction* Func = Conv->GetConversionFunction().GetFunction(WB))
		{
			auto Args = UE::MVVM::BindingHelper::TryGetArgumentsForConversionFunction(Func);
			if (Args.HasValue())
			{
				for (const FProperty* Arg : Args.GetValue())
				{
					TArray<FName> Names = { Arg->GetFName() };
					const FMVVMBlueprintPropertyPath P = Sub->GetPathForConversionFunctionArgument(WB, B, FMVVMBlueprintPinId(MoveTemp(Names)), true);
					if (P.IsValid() && P.GetViewModelId().IsValid()) { return P; }
				}
			}
		}
		return FMVVMBlueprintPropertyPath();
	}

	bool SameSource(const FMVVMBlueprintPropertyPath& A, const FMVVMBlueprintPropertyPath& B)
	{
		return A.GetViewModelId() == B.GetViewModelId() && FieldText(A).Equals(FieldText(B), ESearchCase::IgnoreCase);
	}

	bool SameDestination(const FMVVMBlueprintPropertyPath& A, const FMVVMBlueprintPropertyPath& B)
	{
		return A.GetWidgetName() == B.GetWidgetName() && FieldText(A).Equals(FieldText(B), ESearchCase::IgnoreCase);
	}

	bool IsNotify(const UClass* Class, FName Field)
	{
		if (const INotifyFieldValueChanged* N = Cast<INotifyFieldValueChanged>(Class->GetDefaultObject()))
		{
			return N->GetFieldNotificationDescriptor().GetField(Class, Field).IsValid();
		}
		return false;
	}

	// ---- classes and view models

	bool IsViewModelClass(const UClass* C)
	{
		return C && C->ImplementsInterface(UNotifyFieldValueChanged::StaticClass()) && !C->IsChildOf(UWidget::StaticClass())
			&& !C->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists) && !C->GetName().StartsWith(TEXT("SKEL_")) && !C->GetName().StartsWith(TEXT("REINST_"));
	}

	TArray<FString> ViewModelClassNames()
	{
		TArray<FString> Out;
		for (TObjectIterator<UClass> It; It; ++It) { if (IsViewModelClass(*It)) { Out.Add(It->GetName()); } }
		Out.Sort();
		return Out;
	}

	UClass* FindViewModelClass(const FString& In, FEWResult& OutErr)
	{
		UClass* C = nullptr;
		if (In.StartsWith(TEXT("/Script/"))) { C = FindObject<UClass>(nullptr, *In); }
		else if (In.StartsWith(TEXT("/")))
		{
			FString Obj = In;
			if (!Obj.Contains(TEXT("."))) { Obj = In + TEXT(".") + FPackageName::GetShortName(In) + TEXT("_C"); }
			else if (!Obj.EndsWith(TEXT("_C"))) { Obj += TEXT("_C"); }
			C = LoadObject<UClass>(nullptr, *Obj, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		else
		{
			TArray<UClass*> Hits;
			const FString WithoutPrefix = In.Len() > 1 && In[0] == TEXT('U') ? In.Mid(1) : In;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (IsViewModelClass(*It) && (It->GetName().Equals(In, ESearchCase::IgnoreCase) || It->GetName().Equals(WithoutPrefix, ESearchCase::IgnoreCase))) { Hits.Add(*It); }
			}
			if (Hits.Num() > 1)
			{
				TArray<FString> Paths;
				for (UClass* H : Hits) { Paths.Add(H->GetPathName()); }
				OutErr = Bad(TEXT("AMBIGUOUS_CLASS"), FString::Printf(TEXT("More than one view model class is called '%s'. Use the full path."), *In), Paths);
				return nullptr;
			}
			if (Hits.Num() == 1) { C = Hits[0]; }
		}
		if (!C)
		{
			OutErr = Bad(TEXT("CLASS_NOT_FOUND"), FString::Printf(TEXT("No view model class '%s'. Use a /Script/Module.Class path, a short C++ class name or a Blueprint class path."), *In), Near(In, ViewModelClassNames()));
			return nullptr;
		}
		if (!IsViewModelClass(C))
		{
			OutErr = Bad(TEXT("INVALID_VIEWMODEL"), FString::Printf(TEXT("%s is not a view model class: it must derive from MVVMViewModelBase (or implement INotifyFieldValueChanged), must not be abstract and must not be a widget."), *C->GetName()), Near(In, ViewModelClassNames()));
			return nullptr;
		}
		return C;
	}

	UMVVMBlueprintView* GetView(UWidgetBlueprint* WB)
	{
		UMVVMEditorSubsystem* Sub = Subsystem();
		return Sub ? Sub->GetView(WB) : nullptr;
	}

	TArray<FString> ViewModelNames(UMVVMBlueprintView* View)
	{
		TArray<FString> Out;
		if (View) { for (const FMVVMBlueprintViewModelContext& C : View->GetViewModels()) { Out.Add(C.GetViewModelName().ToString()); } }
		return Out;
	}

	/** The widget's view model called In, or the only one of class In. */
	const FMVVMBlueprintViewModelContext* FindContext(UMVVMBlueprintView* View, const FString& In, FEWResult& OutErr)
	{
		if (!View || View->GetViewModels().Num() == 0)
		{
			OutErr = Bad(TEXT("VIEWMODEL_NOT_FOUND"), TEXT("The Widget Blueprint has no view models. Add one with viewmodel_add."));
			return nullptr;
		}
		for (const FMVVMBlueprintViewModelContext& C : View->GetViewModels())
		{
			if (C.GetViewModelName().ToString().Equals(In, ESearchCase::IgnoreCase)) { return &C; }
		}
		const FString WithoutPrefix = In.Len() > 1 && In[0] == TEXT('U') ? In.Mid(1) : In;
		const FMVVMBlueprintViewModelContext* Found = nullptr;
		int32 Count = 0;
		for (const FMVVMBlueprintViewModelContext& C : View->GetViewModels())
		{
			const UClass* Class = C.GetViewModelClass();
			if (Class && (Class->GetPathName().Equals(In, ESearchCase::IgnoreCase) || Class->GetName().Equals(In, ESearchCase::IgnoreCase) || Class->GetName().Equals(WithoutPrefix, ESearchCase::IgnoreCase)))
			{
				Found = &C;
				++Count;
			}
		}
		if (Count == 1) { return Found; }
		if (Count > 1) { OutErr = Bad(TEXT("AMBIGUOUS_VIEWMODEL"), FString::Printf(TEXT("More than one view model is of class '%s'. Use its name."), *In), ViewModelNames(View)); return nullptr; }
		OutErr = Bad(TEXT("VIEWMODEL_NOT_FOUND"), FString::Printf(TEXT("The widget has no view model '%s'."), *In), ViewModelNames(View));
		return nullptr;
	}

	// ---- property paths

	/** Resolves "A.B.C" from Start (the first name may be a function). Fields are appended to Out. */
	bool ResolvePath(const UStruct* Start, const FString& Path, const TCHAR* What, TArray<FMVVMConstFieldVariant>& Out, FEWResult& OutErr)
	{
		TArray<FString> Segments;
		Path.ParseIntoArray(Segments, TEXT("."));
		if (Segments.Num() == 0) { OutErr = Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("%s is empty."), What)); return false; }
		const UStruct* Cur = Start;
		for (int32 i = 0; i < Segments.Num(); ++i)
		{
			if (!Cur)
			{
				OutErr = Bad(TEXT("PROPERTY_NOT_FOUND"), FString::Printf(TEXT("%s: '%s' is not a struct or object property, so '%s' cannot be reached through it."), What, *Segments[i - 1], *Segments[i]));
				return false;
			}
			const FName Name(*Segments[i]);
			if (const FProperty* P = Cur->FindPropertyByName(Name))
			{
				Out.Add(FMVVMConstFieldVariant(P));
				if (const FStructProperty* S = CastField<FStructProperty>(P)) { Cur = S->Struct; }
				else if (const FObjectProperty* O = CastField<FObjectProperty>(P)) { Cur = O->PropertyClass; }
				else { Cur = nullptr; }
				continue;
			}
			if (const UClass* AsClass = Cast<UClass>(Cur))
			{
				if (const UFunction* F = AsClass->FindFunctionByName(Name))
				{
					Out.Add(FMVVMConstFieldVariant(F));
					Cur = nullptr;
					continue;
				}
			}
			TArray<FString> Names;
			for (TFieldIterator<FProperty> It(Cur); It; ++It) { Names.Add(It->GetName()); }
			if (const UClass* AsClass = Cast<UClass>(Cur)) { for (TFieldIterator<UFunction> It(AsClass); It; ++It) { if (It->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure)) { Names.Add(It->GetName()); } } }
			OutErr = Bad(TEXT("PROPERTY_NOT_FOUND"), FString::Printf(TEXT("%s: no property or function '%s' on %s."), What, *Segments[i], *Cur->GetName()), Near(Segments[i], Names));
			return false;
		}
		return true;
	}

	const FProperty* SourceType(const FMVVMConstFieldVariant& Field, FString& OutErr)
	{
		auto R = UE::MVVM::BindingHelper::TryGetPropertyTypeForSourceBinding(Field);
		if (R.HasError()) { OutErr = R.GetError().ToString(); return nullptr; }
		return R.GetValue();
	}

	const FProperty* DestinationType(const FMVVMConstFieldVariant& Field, FString& OutErr)
	{
		auto R = UE::MVVM::BindingHelper::TryGetPropertyTypeForDestinationBinding(Field);
		if (R.HasError()) { OutErr = R.GetError().ToString(); return nullptr; }
		return R.GetValue();
	}

	FString TypeText(const FProperty* P)
	{
		if (!P) { return TEXT("?"); }
		FString Extended;
		const FString Base = P->GetCPPType(&Extended);
		return Base + Extended;
	}

	const UFunction* FindConversionFunction(const FString& In, FEWResult& OutErr)
	{
		const UFunction* Found = nullptr;
		int32 Colon = INDEX_NONE;
		if (In.FindChar(TEXT(':'), Colon))
		{
			const FString ClassPart = In.Left(Colon), FnPart = In.Mid(Colon + 1);
			UClass* C = FindObject<UClass>(nullptr, *ClassPart);
			if (!C) { C = LoadObject<UClass>(nullptr, *ClassPart, nullptr, LOAD_NoWarn | LOAD_Quiet); }
			if (C) { Found = C->FindFunctionByName(FName(*FnPart)); }
		}
		else
		{
			for (TObjectIterator<UClass> It; It && !Found; ++It)
			{
				if (It->IsChildOf(UBlueprintFunctionLibrary::StaticClass())) { Found = It->FindFunctionByName(FName(*In), EIncludeSuperFlag::ExcludeSuper); }
			}
		}
		if (!Found) { OutErr = Bad(TEXT("CONVERSION_NOT_FOUND"), FString::Printf(TEXT("No conversion function '%s'. Use a static Blueprint function, e.g. Conv_IntToText or /Script/Engine.KismetTextLibrary:Conv_IntToText."), *In)); }
		return Found;
	}

	// ---- reading

	TSharedRef<FJsonObject> BindingJson(UWidgetBlueprint* WB, UMVVMBlueprintView* View, const FMVVMBlueprintViewBinding& B)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("id"), B.BindingId.ToString(EGuidFormats::DigitsWithHyphens));
		const FMVVMBlueprintPropertyPath Source = EffectiveSource(WB, B);
		FString VmName;
		if (const FMVVMBlueprintViewModelContext* C = View->FindViewModel(Source.GetViewModelId())) { VmName = C->GetViewModelName().ToString(); }
		else if (!Source.GetWidgetName().IsNone()) { VmName = Source.GetWidgetName().ToString(); }
		const FString Field = FieldText(Source);
		J->SetStringField(TEXT("viewModel"), VmName);
		J->SetStringField(TEXT("sourceField"), Field);
		J->SetStringField(TEXT("source"), VmName.IsEmpty() ? Field : VmName + TEXT(".") + Field);
		const FString TargetWidget = B.DestinationPath.GetWidgetName().ToString();
		const FString TargetField = FieldText(B.DestinationPath);
		J->SetStringField(TEXT("target"), TargetWidget.IsEmpty() ? TargetField : TargetWidget + TEXT(".") + TargetField);
		J->SetStringField(TEXT("mode"), StaticEnum<EMVVMBindingMode>()->GetNameStringByValue((int64)B.BindingType));
		FString Conversion;
		if (B.Conversion.SourceToDestinationConversion) { Conversion = B.Conversion.SourceToDestinationConversion->GetConversionFunction().GetName().ToString(); }
		J->SetStringField(TEXT("conversion"), Conversion);
		J->SetBoolField(TEXT("enabled"), B.bEnabled);
		J->SetBoolField(TEXT("compile"), B.bCompile);
		TArray<TSharedPtr<FJsonValue>> Errors, Warnings;
		for (const FText& T : View->GetBindingMessages(B.BindingId, UE::MVVM::EBindingMessageType::Error)) { Errors.Add(Str(T.ToString())); }
		for (const FText& T : View->GetBindingMessages(B.BindingId, UE::MVVM::EBindingMessageType::Warning)) { Warnings.Add(Str(T.ToString())); }
		J->SetArrayField(TEXT("errors"), Errors);
		J->SetArrayField(TEXT("warnings"), Warnings);
		return J;
	}

	TSharedRef<FJsonObject> ViewModelJson(const FMVVMBlueprintViewModelContext& C)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("name"), C.GetViewModelName().ToString());
		J->SetStringField(TEXT("class"), ClassPath(C.GetViewModelClass()));
		J->SetStringField(TEXT("creationType"), StaticEnum<EMVVMBlueprintViewModelContextCreationType>()->GetNameStringByValue((int64)C.CreationType));
		J->SetStringField(TEXT("id"), C.GetViewModelId().ToString(EGuidFormats::DigitsWithHyphens));
		return J;
	}

	FEWResult OpList(UWidgetBlueprint* WB, const TSharedPtr<FJsonObject>&)
	{
		TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
		D->SetStringField(TEXT("widget"), WB->GetPathName());
		TArray<TSharedPtr<FJsonValue>> Vms, Bindings;
		if (UMVVMBlueprintView* View = GetView(WB))
		{
			for (const FMVVMBlueprintViewModelContext& C : View->GetViewModels()) { Vms.Add(MakeShared<FJsonValueObject>(ViewModelJson(C))); }
			for (const FMVVMBlueprintViewBinding& B : View->GetBindings()) { Bindings.Add(MakeShared<FJsonValueObject>(BindingJson(WB, View, B))); }
		}
		D->SetNumberField(TEXT("viewModelCount"), Vms.Num());
		D->SetNumberField(TEXT("bindingCount"), Bindings.Num());
		D->SetArrayField(TEXT("viewModels"), Vms);
		D->SetArrayField(TEXT("bindings"), Bindings);
		return FEWResult::Ok(D);
	}

	// ---- writing

	FEWResult OpAdd(UWidgetBlueprint* WB, const TSharedPtr<FJsonObject>& Args)
	{
		UMVVMEditorSubsystem* Sub = Subsystem();
		if (!Sub) { return Bad(TEXT("MVVM_NOT_ENABLED"), TEXT("The MVVM editor subsystem is not available.")); }

		FEWResult Err;
		UClass* Class = FindViewModelClass(Opt(Args, TEXT("viewModel")), Err);
		if (!Class) { return Err; }

		FString Name = Opt(Args, TEXT("name"));
		if (Name.IsEmpty()) { Name = UMVVMEditorSubsystem::GetDefaultViewModelName(Class); }

		const FString CreationText = Opt(Args, TEXT("creationType"));
		EMVVMBlueprintViewModelContextCreationType Type = EMVVMBlueprintViewModelContextCreationType::CreateInstance;
		if (!CreationText.IsEmpty())
		{
			const int64 V = StaticEnum<EMVVMBlueprintViewModelContextCreationType>()->GetValueByNameString(CreationText);
			if (V == INDEX_NONE) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Unknown creationType '%s'."), *CreationText), { TEXT("CreateInstance"), TEXT("Manual"), TEXT("GlobalViewModelCollection"), TEXT("PropertyPath") }); }
			Type = (EMVVMBlueprintViewModelContextCreationType)V;
		}
		const TArray<EMVVMBlueprintViewModelContextCreationType> Allowed = UE::MVVM::GetAllowedContextCreationType(Class);
		if (!Allowed.Contains(Type))
		{
			TArray<FString> Names;
			for (EMVVMBlueprintViewModelContextCreationType A : Allowed) { Names.Add(StaticEnum<EMVVMBlueprintViewModelContextCreationType>()->GetNameStringByValue((int64)A)); }
			return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("%s cannot use creationType %s."), *Class->GetName(), *StaticEnum<EMVVMBlueprintViewModelContextCreationType>()->GetNameStringByValue((int64)Type)), Names);
		}
		const FString GlobalId = Opt(Args, TEXT("globalIdentifier")), PropertyPath = Opt(Args, TEXT("propertyPath"));
		if (Type == EMVVMBlueprintViewModelContextCreationType::GlobalViewModelCollection && GlobalId.IsEmpty()) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("creationType GlobalViewModelCollection needs globalIdentifier.")); }
		if (Type == EMVVMBlueprintViewModelContextCreationType::PropertyPath && PropertyPath.IsEmpty()) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("creationType PropertyPath needs propertyPath.")); }

		UMVVMBlueprintView* View = GetView(WB);
		if (View)
		{
			if (const FMVVMBlueprintViewModelContext* Existing = View->FindViewModel(FName(*Name)))
			{
				if (Existing->GetViewModelClass() == Class)
				{
					TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
					D->SetObjectField(TEXT("viewModel"), ViewModelJson(*Existing));
					D->SetBoolField(TEXT("added"), false);
					D->SetBoolField(TEXT("changed"), false);
					return FEWResult::Ok(D, TEXT("The view model is already in the widget; nothing changed."));
				}
				return Bad(TEXT("VIEWMODEL_EXISTS"), FString::Printf(TEXT("The widget already has a view model '%s' of class %s. Pick another name."), *Name, *ClassPath(Existing->GetViewModelClass())));
			}
		}
		if (FKismetNameValidator(WB).IsValid(Name) != EValidatorResult::Ok)
		{
			return Bad(TEXT("INVALID_NAME"), FString::Printf(TEXT("'%s' cannot be used: the Widget Blueprint already has a variable, widget or function with this name, or the name is not valid."), *Name));
		}

		// All checks passed: write.
		View = Sub->RequestView(WB);
		if (!View) { return Bad(TEXT("MVVM_FAILED"), TEXT("The MVVM view of the Widget Blueprint could not be created.")); }
		FMVVMBlueprintViewModelContext Context(Class, FName(*Name));
		if (!Context.IsValid()) { return Bad(TEXT("MVVM_FAILED"), TEXT("The editor refused the view model.")); }
		Context.Resolver = Context.CreateDefaultResolver(WB);
		Context.CreationType = Type;
		Context.bUseAsInterface = false;
		if (Type == EMVVMBlueprintViewModelContextCreationType::Manual) { Context.bOptional = true; Context.bCreateSetterFunction = true; }
		if (Type == EMVVMBlueprintViewModelContextCreationType::GlobalViewModelCollection) { Context.GlobalViewModelIdentifier = FName(*GlobalId); }
		if (Type == EMVVMBlueprintViewModelContextCreationType::PropertyPath) { Context.ViewModelPropertyPath = PropertyPath; }
		View->Modify();
		View->AddViewModel(Context);

		const FMVVMBlueprintViewModelContext* Now = View->FindViewModel(FName(*Name));
		if (!Now || Now->GetViewModelClass() != Class) { return Bad(TEXT("MVVM_FAILED"), TEXT("The view model was not added.")); }
		TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
		D->SetObjectField(TEXT("viewModel"), ViewModelJson(*Now));
		D->SetBoolField(TEXT("added"), true);
		D->SetBoolField(TEXT("changed"), true);
		return FEWResult::Ok(D);
	}

	/** target "Gold.Text": a widget of the tree and a property (or a function) of its class. */
	bool ParseTarget(UWidgetBlueprint* WB, const FString& Target, FString& OutWidget, FString& OutField, FEWResult& OutErr)
	{
		int32 Dot = INDEX_NONE;
		if (!Target.FindChar(TEXT('.'), Dot) || Dot == 0 || Dot == Target.Len() - 1)
		{
			OutErr = Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("target must be 'Widget.Property', e.g. Gold.Text (got '%s')."), *Target));
			return false;
		}
		OutWidget = Target.Left(Dot);
		OutField = Target.Mid(Dot + 1);
		return true;
	}

	TArray<FString> WidgetNames(UWidgetBlueprint* WB)
	{
		TArray<UWidget*> All;
		WB->WidgetTree->GetAllWidgets(All);
		TArray<FString> Out;
		for (UWidget* W : All) { if (W) { Out.Add(W->GetName()); } }
		return Out;
	}

	FEWResult OpBind(UWidgetBlueprint* WB, const TSharedPtr<FJsonObject>& Args)
	{
		UMVVMEditorSubsystem* Sub = Subsystem();
		if (!Sub) { return Bad(TEXT("MVVM_NOT_ENABLED"), TEXT("The MVVM editor subsystem is not available.")); }
		if (!WB->WidgetTree) { return Bad(TEXT("INVALID_ASSET"), TEXT("The Widget Blueprint has no widget tree.")); }
		UMVVMBlueprintView* View = GetView(WB);

		FEWResult Err;
		const FMVVMBlueprintViewModelContext* Context = FindContext(View, Opt(Args, TEXT("viewModel")), Err);
		if (!Context) { return Err; }
		const UClass* VmClass = Context->GetViewModelClass();
		if (!VmClass) { return Bad(TEXT("INVALID_VIEWMODEL"), TEXT("The view model has no class (it may have been deleted).")); }

		// mode
		EMVVMBindingMode Mode = EMVVMBindingMode::OneWayToDestination;
		const FString ModeText = Opt(Args, TEXT("mode"));
		if (!ModeText.IsEmpty())
		{
			const int64 V = StaticEnum<EMVVMBindingMode>()->GetValueByNameString(ModeText);
			if (V == INDEX_NONE) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Unknown mode '%s'."), *ModeText), { TEXT("OneTimeToDestination"), TEXT("OneWayToDestination"), TEXT("TwoWay"), TEXT("OneWayToSource") }); }
			Mode = (EMVVMBindingMode)V;
		}
		const bool bForward = UE::MVVM::IsForwardBinding(Mode), bBackward = UE::MVVM::IsBackwardBinding(Mode);

		// source: a view model property or function
		const FString SourceText = Opt(Args, TEXT("source"));
		TArray<FMVVMConstFieldVariant> SourceFields;
		if (!ResolvePath(VmClass, SourceText, TEXT("source"), SourceFields, Err)) { return Err; }
		FString TypeErr;
		const FProperty* SourceProp = SourceType(SourceFields.Last(), TypeErr);
		if (!SourceProp) { return Bad(TEXT("INVALID_SOURCE"), FString::Printf(TEXT("'%s' cannot be the source of a binding: %s"), *SourceText, *TypeErr)); }
		if (bForward && Mode != EMVVMBindingMode::OneTimeToDestination)
		{
			TArray<FString> Parts;
			SourceText.ParseIntoArray(Parts, TEXT("."));
			if (Parts.Num() == 1 && !IsNotify(VmClass, FName(*Parts[0])))
			{
				return Bad(TEXT("SOURCE_NOT_NOTIFY"), FString::Printf(TEXT("'%s' of %s does not broadcast changes (it is not a FieldNotify property or function), so a %s binding would never update. Mark it FieldNotify in the view model class, or use mode OneTimeToDestination."), *SourceText, *VmClass->GetName(), *StaticEnum<EMVVMBindingMode>()->GetNameStringByValue((int64)Mode)));
			}
		}

		// target: widget + property
		FString WidgetName, FieldText_;
		if (!ParseTarget(WB, Opt(Args, TEXT("target")), WidgetName, FieldText_, Err)) { return Err; }
		UWidget* Widget = WB->WidgetTree->FindWidget(FName(*WidgetName));
		if (!Widget) { return Bad(TEXT("WIDGET_NOT_FOUND"), FString::Printf(TEXT("No widget '%s' in %s."), *WidgetName, *WB->GetName()), Near(WidgetName, WidgetNames(WB))); }
		TArray<FMVVMConstFieldVariant> TargetFields;
		if (!ResolvePath(Widget->GetClass(), FieldText_, TEXT("target"), TargetFields, Err)) { return Err; }
		const FProperty* TargetProp = DestinationType(TargetFields.Last(), TypeErr);
		if (!TargetProp && bForward) { return Bad(TEXT("INVALID_TARGET"), FString::Printf(TEXT("'%s' cannot be the target of a binding: %s"), *Opt(Args, TEXT("target")), *TypeErr)); }
		if (!TargetProp) { TargetProp = SourceType(TargetFields.Last(), TypeErr); }
		if (!TargetProp) { return Bad(TEXT("INVALID_TARGET"), FString::Printf(TEXT("'%s' cannot be bound: %s"), *Opt(Args, TEXT("target")), *TypeErr)); }

		// types and conversion
		const FString ConversionText = Opt(Args, TEXT("conversion"));
		const UFunction* Conversion = nullptr;
		const bool bCompatible = UE::MVVM::BindingHelper::ArePropertiesCompatible(SourceProp, TargetProp);
		if (!ConversionText.IsEmpty())
		{
			if (Mode != EMVVMBindingMode::OneWayToDestination && Mode != EMVVMBindingMode::OneTimeToDestination) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("A conversion function is supported only with OneWayToDestination and OneTimeToDestination.")); }
			Conversion = FindConversionFunction(ConversionText, Err);
			if (!Conversion) { return Err; }
			if (!Sub->IsValidConversionFunction(WB, Conversion, SourceProp, TargetProp))
			{
				TArray<FString> Fits;
				for (const UE::MVVM::FConversionFunctionValue& F : Sub->GetConversionFunctions(WB, SourceProp, TargetProp)) { Fits.Add(F.GetFName().ToString()); }
				Fits.Sort();
				return Bad(TEXT("TYPE_MISMATCH"), FString::Printf(TEXT("%s cannot convert %s to %s."), *ConversionText, *TypeText(SourceProp), *TypeText(TargetProp)), Near(ConversionText, Fits));
			}
		}
		else if (!bCompatible || (bBackward && bForward && !UE::MVVM::BindingHelper::ArePropertiesCompatible(TargetProp, SourceProp)))
		{
			TArray<FString> Fits;
			if (Mode == EMVVMBindingMode::OneWayToDestination || Mode == EMVVMBindingMode::OneTimeToDestination)
			{
				for (const UE::MVVM::FConversionFunctionValue& F : Sub->GetConversionFunctions(WB, SourceProp, TargetProp)) { Fits.Add(F.GetFName().ToString()); }
				Fits.Sort();
			}
			return Bad(TEXT("TYPE_MISMATCH"), FString::Printf(TEXT("'%s' is %s but '%s' is %s. Pass a conversion function (conversion), for example one of the suggestions."), *SourceText, *TypeText(SourceProp), *Opt(Args, TEXT("target")), *TypeText(TargetProp)), Near(TEXT("Conv_"), Fits, 8));
		}

		// existing binding for this target
		FMVVMBlueprintPropertyPath SourcePath, DestinationPath;
		SourcePath.SetViewModelId(Context->GetViewModelId());
		for (const FMVVMConstFieldVariant& F : SourceFields) { SourcePath.AppendPropertyPath(WB, F); }
		DestinationPath.SetWidgetName(Widget->GetFName());
		for (const FMVVMConstFieldVariant& F : TargetFields) { DestinationPath.AppendPropertyPath(WB, F); }

		const FMVVMBlueprintViewBinding* Existing = nullptr;
		for (const FMVVMBlueprintViewBinding& B : View->GetBindings())
		{
			if (SameDestination(B.DestinationPath, DestinationPath)) { Existing = &B; break; }
		}
		if (Existing && !SameSource(EffectiveSource(WB, *Existing), SourcePath))
		{
			const FMVVMBlueprintPropertyPath Other = EffectiveSource(WB, *Existing);
			FString OtherVm;
			if (const FMVVMBlueprintViewModelContext* C = View->FindViewModel(Other.GetViewModelId())) { OtherVm = C->GetViewModelName().ToString() + TEXT("."); }
			return Bad(TEXT("BINDING_EXISTS"), FString::Printf(TEXT("'%s' is already bound to another source (%s%s). Remove it first with viewmodel_unbind."), *Opt(Args, TEXT("target")), *OtherVm, *FieldText(Other)));
		}
		// An existing binding keeps its conversion function when none is given.
		FString ExistingConversion;
		if (Existing && Existing->Conversion.SourceToDestinationConversion) { ExistingConversion = Existing->Conversion.SourceToDestinationConversion->GetConversionFunction().GetName().ToString(); }
		const bool bSameConversion = Conversion ? ExistingConversion.Equals(Conversion->GetName(), ESearchCase::IgnoreCase) : true;
		if (Existing && Existing->BindingType == Mode && bSameConversion)
		{
			TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
			D->SetObjectField(TEXT("binding"), BindingJson(WB, View, *Existing));
			D->SetBoolField(TEXT("added"), false);
			D->SetBoolField(TEXT("changed"), false);
			return FEWResult::Ok(D, TEXT("The binding already exists; nothing changed."));
		}

		// All checks passed: write.
		FGuid Id;
		if (Existing)
		{
			Id = Existing->BindingId;
		}
		else
		{
			FMVVMBlueprintViewBinding& B = Sub->AddBinding(WB);
			Id = B.BindingId;
			Sub->SetSourcePathForBinding(WB, *View->GetBinding(Id), SourcePath);
			Sub->SetDestinationPathForBinding(WB, *View->GetBinding(Id), DestinationPath, false);
		}
		FMVVMBlueprintViewBinding* B = View->GetBinding(Id);
		if (!B) { return Bad(TEXT("MVVM_FAILED"), TEXT("The binding was not created.")); }
		if (B->BindingType != Mode)
		{
			Sub->SetBindingTypeForBinding(WB, *B, Mode);
			B = View->GetBinding(Id);
		}
		if (Conversion && B && (!Existing || !bSameConversion))
		{
			// With a conversion function the source is not the binding's SourcePath but the argument pin of the function.
			Sub->SetSourceToDestinationConversionFunction(WB, *B, FMVVMBlueprintFunctionReference(WB, Conversion));
			B = View->GetBinding(Id);
			const TArray<FMVVMBlueprintPinId> Pins = B ? Sub->GetConversionFunctionPins(WB, UE::MVVM::FConversionFunctionValue(Conversion), SourceProp) : TArray<FMVVMBlueprintPinId>();
			if (Pins.IsEmpty()) { return Bad(TEXT("MVVM_FAILED"), FString::Printf(TEXT("%s has no argument that takes %s."), *ConversionText, *TypeText(SourceProp))); }
			Sub->SetPathForConversionFunctionArgument(WB, *B, Pins[0], SourcePath, true);
			B = View->GetBinding(Id);
		}
		if (!B) { return Bad(TEXT("MVVM_FAILED"), TEXT("The binding was lost while it was set up.")); }
		if (!SameSource(EffectiveSource(WB, *B), SourcePath) || !SameDestination(B->DestinationPath, DestinationPath) || B->BindingType != Mode) { return Bad(TEXT("MVVM_FAILED"), TEXT("The binding read back differently from what was set.")); }

		TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
		D->SetObjectField(TEXT("binding"), BindingJson(WB, View, *B));
		D->SetBoolField(TEXT("added"), Existing == nullptr);
		D->SetBoolField(TEXT("changed"), true);
		return FEWResult::Ok(D);
	}

	FEWResult OpUnbind(UWidgetBlueprint* WB, const TSharedPtr<FJsonObject>& Args)
	{
		UMVVMEditorSubsystem* Sub = Subsystem();
		if (!Sub) { return Bad(TEXT("MVVM_NOT_ENABLED"), TEXT("The MVVM editor subsystem is not available.")); }
		UMVVMBlueprintView* View = GetView(WB);

		FEWResult Err;
		FString WidgetName, Field;
		if (!ParseTarget(WB, Opt(Args, TEXT("target")), WidgetName, Field, Err)) { return Err; }
		FGuid OnlyViewModel;
		if (!Opt(Args, TEXT("viewModel")).IsEmpty())
		{
			const FMVVMBlueprintViewModelContext* C = FindContext(View, Opt(Args, TEXT("viewModel")), Err);
			if (!C) { return Err; }
			OnlyViewModel = C->GetViewModelId();
		}
		const FString OnlySource = Opt(Args, TEXT("source"));

		TArray<FGuid> Ids;
		TArray<FString> Targets;
		if (View)
		{
			for (const FMVVMBlueprintViewBinding& B : View->GetBindings())
			{
				Targets.AddUnique(B.DestinationPath.GetWidgetName().ToString() + TEXT(".") + FieldText(B.DestinationPath));
				if (!B.DestinationPath.GetWidgetName().ToString().Equals(WidgetName, ESearchCase::IgnoreCase) || !FieldText(B.DestinationPath).Equals(Field, ESearchCase::IgnoreCase)) { continue; }
				const FMVVMBlueprintPropertyPath Source = EffectiveSource(WB, B);
				if (OnlyViewModel.IsValid() && Source.GetViewModelId() != OnlyViewModel) { continue; }
				if (!OnlySource.IsEmpty() && !FieldText(Source).Equals(OnlySource, ESearchCase::IgnoreCase)) { continue; }
				Ids.Add(B.BindingId);
			}
		}
		if (Ids.Num() == 0) { return Bad(TEXT("BINDING_NOT_FOUND"), FString::Printf(TEXT("No binding matches '%s'."), *Opt(Args, TEXT("target"))), Targets); }

		TArray<TSharedPtr<FJsonValue>> Removed;
		for (const FGuid& Id : Ids)
		{
			if (const FMVVMBlueprintViewBinding* B = View->GetBinding(Id))
			{
				Removed.Add(MakeShared<FJsonValueObject>(BindingJson(WB, View, *B)));
				Sub->RemoveBinding(WB, *B);
			}
		}
		for (const FGuid& Id : Ids) { if (View->GetBinding(Id)) { return Bad(TEXT("MVVM_FAILED"), TEXT("A binding was not removed.")); } }
		TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
		D->SetArrayField(TEXT("removed"), Removed);
		D->SetBoolField(TEXT("changed"), true);
		return FEWResult::Ok(D);
	}

	FEWResult OpRemove(UWidgetBlueprint* WB, const TSharedPtr<FJsonObject>& Args)
	{
		UMVVMEditorSubsystem* Sub = Subsystem();
		if (!Sub) { return Bad(TEXT("MVVM_NOT_ENABLED"), TEXT("The MVVM editor subsystem is not available.")); }
		UMVVMBlueprintView* View = GetView(WB);

		FEWResult Err;
		const FMVVMBlueprintViewModelContext* Context = FindContext(View, Opt(Args, TEXT("viewModel")), Err);
		if (!Context) { return Err; }
		if (!Context->bCanRemove) { return Bad(TEXT("CANNOT_REMOVE"), FString::Printf(TEXT("The view model '%s' is locked (it cannot be removed in the editor either)."), *Context->GetViewModelName().ToString())); }
		const FGuid Id = Context->GetViewModelId();
		const FName Name = Context->GetViewModelName();
		const TSharedRef<FJsonObject> Gone = ViewModelJson(*Context);

		TArray<FGuid> Users;
		TArray<FString> UserTexts;
		for (const FMVVMBlueprintViewBinding& B : View->GetBindings())
		{
			const FMVVMBlueprintPropertyPath Source = EffectiveSource(WB, B);
			if (Source.GetViewModelId() == Id || B.DestinationPath.GetViewModelId() == Id)
			{
				Users.Add(B.BindingId);
				UserTexts.Add(FieldText(Source) + TEXT(" -> ") + B.DestinationPath.GetWidgetName().ToString() + TEXT(".") + FieldText(B.DestinationPath));
			}
		}
		bool bRemoveBindings = false;
		if (Args->HasField(TEXT("removeBindings"))) { Args->TryGetBoolField(TEXT("removeBindings"), bRemoveBindings); }
		if (Users.Num() && !bRemoveBindings)
		{
			return Bad(TEXT("VIEWMODEL_IN_USE"), FString::Printf(TEXT("%d binding(s) still use '%s'. Remove them with viewmodel_unbind, or pass removeBindings: true."), Users.Num(), *Name.ToString()), UserTexts);
		}

		for (const FGuid& U : Users) { if (const FMVVMBlueprintViewBinding* B = View->GetBinding(U)) { Sub->RemoveBinding(WB, *B); } }
		Sub->RemoveViewModel(WB, Name);
		if (View->FindViewModel(Name)) { return Bad(TEXT("MVVM_FAILED"), TEXT("The view model was not removed.")); }

		TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
		D->SetObjectField(TEXT("removed"), Gone);
		D->SetNumberField(TEXT("bindingsRemoved"), Users.Num());
		D->SetBoolField(TEXT("changed"), true);
		return FEWResult::Ok(D);
	}
}

class FEnginewrightMVVMModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FEWMVVMBackend::Set(TEXT("list"), &OpList);
		FEWMVVMBackend::Set(TEXT("add"), &OpAdd);
		FEWMVVMBackend::Set(TEXT("bind"), &OpBind);
		FEWMVVMBackend::Set(TEXT("unbind"), &OpUnbind);
		FEWMVVMBackend::Set(TEXT("remove"), &OpRemove);
		UE_LOG(LogTemp, Display, TEXT("Enginewright: MVVM support on (viewmodel_* tools)."));
	}

	virtual void ShutdownModule() override
	{
		FEWMVVMBackend::Clear();
	}
};

IMPLEMENT_MODULE(FEnginewrightMVVMModule, EnginewrightMVVM)
