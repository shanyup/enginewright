// Runtime UI in Play In Editor: what a player's screen shows (UMG and CommonUI: activatable widget stacks,
// buttons with labels and positions) and clicking a button like a player. CommonUI is used through
// reflection, so projects without the CommonUI plugin still work.
#include "EnginewrightTool.h"
#include "EWCommon.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Engine/DataTable.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "UObject/StructOnScope.h"
#include "WidgetBlueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "Input/Events.h"
#include "Layout/WidgetPath.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

namespace
{
	FEWResult Bad(const FString& Code, const FString& Msg, const TArray<FString>& Sug = {}) { return FEWResult::Error(Code, Msg, Sug); }

	UClass* CommonClass(const TCHAR* Name) { return FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/CommonUI.%s"), Name)); }

	bool CallBool(UObject* Obj, const TCHAR* Fn)
	{
		UFunction* F = Obj ? Obj->FindFunction(FName(Fn)) : nullptr;
		if (!F || F->ParmsSize > 64) { return false; }
		uint8 Buf[64] = {};
		Obj->ProcessEvent(F, Buf);
		const FBoolProperty* Ret = CastField<FBoolProperty>(F->GetReturnProperty());
		return Ret && Ret->GetPropertyValue_InContainer(Buf);
	}

	/** Call a UFunction whose first input parameter is an object or class; returns its object return value. */
	UObject* CallWithObject(UObject* Obj, const TCHAR* Fn, UObject* Arg, bool* bCalled = nullptr)
	{
		UFunction* F = Obj ? Obj->FindFunction(FName(Fn)) : nullptr;
		if (bCalled) { *bCalled = F != nullptr; }
		if (!F) { return nullptr; }
		FStructOnScope Parms(F);
		for (TFieldIterator<FProperty> It(F); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_ReturnParm)) { continue; }
			if (FObjectPropertyBase* OP = CastField<FObjectPropertyBase>(*It)) { OP->SetObjectPropertyValue_InContainer(Parms.GetStructMemory(), Arg); }
			break;
		}
		Obj->ProcessEvent(F, Parms.GetStructMemory());
		const FObjectPropertyBase* Ret = CastField<FObjectPropertyBase>(F->GetReturnProperty());
		return Ret ? Ret->GetObjectPropertyValue_InContainer(Parms.GetStructMemory()) : nullptr;
	}

	/** The integer value of an enum (or byte) property. */
	int64 EnumValue(const FProperty* P, const void* Container)
	{
		if (const FEnumProperty* EP = CastField<FEnumProperty>(P)) { return EP->GetUnderlyingProperty()->GetSignedIntPropertyValue(EP->ContainerPtrToValuePtr<void>(Container)); }
		if (const FNumericProperty* NP = CastField<FNumericProperty>(P)) { return NP->GetSignedIntPropertyValue(NP->ContainerPtrToValuePtr<void>(Container)); }
		return -1;
	}

	/** CommonInput's per-player subsystem (null without the CommonUI plugin). */
	UObject* CommonInputSubsystem(APlayerController* PC)
	{
		UClass* C = FindObject<UClass>(nullptr, TEXT("/Script/CommonInput.CommonInputSubsystem"));
		ULocalPlayer* LP = PC ? PC->GetLocalPlayer() : nullptr;
		return (C && LP) ? (UObject*)LP->GetSubsystemBase(TSubclassOf<ULocalPlayerSubsystem>(C)) : nullptr;
	}

	UEnum* CommonInputTypeEnum() { return FindObject<UEnum>(nullptr, TEXT("/Script/CommonInput.ECommonInputType")); }

	/** Current CommonInput input type name (MouseAndKeyboard, Gamepad, Touch), empty without CommonInput. */
	FString InputTypeName(APlayerController* PC)
	{
		UObject* Sub = CommonInputSubsystem(PC);
		UFunction* F = Sub ? Sub->FindFunction(TEXT("GetCurrentInputType")) : nullptr;
		UEnum* E = CommonInputTypeEnum();
		if (!F || !E) { return FString(); }
		FStructOnScope Parms(F);
		Sub->ProcessEvent(F, Parms.GetStructMemory());
		return E->GetNameStringByValue(EnumValue(F->GetReturnProperty(), Parms.GetStructMemory()));
	}

	/** Activatable widget containers (stacks / queues) in this player's world. */
	TArray<UWidget*> Containers(APlayerController* PC)
	{
		TArray<UWidget*> Out;
		UClass* Container = CommonClass(TEXT("CommonActivatableWidgetContainerBase"));
		if (!Container) { return Out; }
		for (TObjectIterator<UWidget> It; It; ++It)
		{
			UWidget* C = *It;
			if (!IsValid(C) || !C->IsA(Container) || C->GetWorld() != PC->GetWorld() || !C->GetCachedWidget().IsValid()) { continue; }
			UUserWidget* Owner = C->GetTypedOuter<UUserWidget>();
			if (Owner && Owner->GetOwningPlayer() && Owner->GetOwningPlayer() != PC) { continue; }
			Out.Add(C);
		}
		return Out;
	}

	TArray<UObject*> StackList(UWidget* C)
	{
		TArray<UObject*> Out;
		if (const FArrayProperty* AP = CastField<FArrayProperty>(C->GetClass()->FindPropertyByName(TEXT("WidgetList"))))
		{
			FScriptArrayHelper H(AP, AP->ContainerPtrToValuePtr<void>(C));
			const FObjectPropertyBase* Inner = CastField<FObjectPropertyBase>(AP->Inner);
			for (int32 i = 0; Inner && i < H.Num(); ++i) { Out.Add(Inner->GetObjectPropertyValue(H.GetRawPtr(i))); }
		}
		return Out;
	}

	/** True when the widget and all its Slate ancestors are visible and it has a size. */
	bool OnScreen(UWidget* W)
	{
		TSharedPtr<SWidget> S = W ? W->GetCachedWidget() : nullptr;
		if (!S.IsValid() || S->GetPaintSpaceGeometry().GetLocalSize().IsNearlyZero()) { return false; }
		for (TSharedPtr<SWidget> P = S; P.IsValid(); P = P->GetParentWidget())
		{
			if (!P->GetVisibility().IsVisible()) { return false; }
		}
		return true;
	}

	TArray<SWidget*> Chain(SWidget* W)
	{
		TArray<SWidget*> Out;
		for (TSharedPtr<SWidget> P = W ? W->AsShared() : TSharedPtr<SWidget>(); P.IsValid(); P = P->GetParentWidget()) { Out.Insert(P.Get(), 0); }
		return Out; // root first
	}

	/** True when A is painted after (on top of) B: compare their branches under the closest common Slate parent. */
	bool PaintedAfter(SWidget* A, SWidget* B)
	{
		const TArray<SWidget*> CA = Chain(A), CB = Chain(B);
		int32 i = 0;
		while (i < CA.Num() && i < CB.Num() && CA[i] == CB[i]) { ++i; }
		if (i == 0 || i >= CA.Num() || i >= CB.Num()) { return false; } // unrelated, or one contains the other
		FChildren* Kids = CA[i - 1]->GetChildren();
		int32 IA = INDEX_NONE, IB = INDEX_NONE;
		for (int32 k = 0; Kids && k < Kids->Num(); ++k)
		{
			const SWidget* K = &Kids->GetChildAt(k).Get();
			if (K == CA[i]) { IA = k; }
			if (K == CB[i]) { IB = k; }
		}
		return IA != INDEX_NONE && IB != INDEX_NONE && IA > IB;
	}

	/** A hit-testable widget drawn over Point on top of Target (UMG level, works without a fresh paint). */
	UWidget* Blocker(UWorld* World, UWidget* Target, const FVector2D& Point)
	{
		SWidget* T = Target->GetCachedWidget().Get();
		for (TObjectIterator<UWidget> It; It; ++It)
		{
			UWidget* W = *It;
			if (!IsValid(W) || W == Target || W->GetWorld() != World || W->GetVisibility() != ESlateVisibility::Visible || !OnScreen(W)) { continue; }
			SWidget* S = W->GetCachedWidget().Get();
			const FGeometry G = S->GetPaintSpaceGeometry();
			const FVector2D TL = G.GetAbsolutePosition(), BR = TL + G.GetAbsoluteSize();
			if (Point.X < TL.X || Point.Y < TL.Y || Point.X > BR.X || Point.Y > BR.Y) { continue; }
			if (PaintedAfter(S, T)) { return W; }
		}
		return nullptr;
	}

	/** The SButton (or subclass, e.g. CommonUI's) at or under W. */
	TSharedPtr<SButton> FindSButton(const TSharedPtr<SWidget>& W, int32 Depth = 0)
	{
		if (!W.IsValid() || Depth > 8) { return nullptr; }
		if (W->GetType().ToString().EndsWith(TEXT("Button"))) { return StaticCastSharedPtr<SButton>(W); }
		FChildren* Kids = W->GetChildren();
		for (int32 k = 0; Kids && k < Kids->Num(); ++k)
		{
			if (TSharedPtr<SButton> B = FindSButton(Kids->GetChildAt(k), Depth + 1)) { return B; }
		}
		return nullptr;
	}

	FString Label(UWidget* W, int32 Depth = 0)
	{
		if (!W || Depth > 12) { return FString(); }
		if (UTextBlock* T = Cast<UTextBlock>(W)) { return T->GetText().ToString(); }
		if (UPanelWidget* P = Cast<UPanelWidget>(W))
		{
			for (int32 i = 0; i < P->GetChildrenCount(); ++i) { const FString L = Label(P->GetChildAt(i), Depth + 1); if (!L.IsEmpty()) { return L; } }
		}
		if (UUserWidget* U = Cast<UUserWidget>(W)) { if (U->WidgetTree) { return Label(U->WidgetTree->RootWidget, Depth + 1); } }
		return FString();
	}

	/** The SViewport geometry of the player's game viewport (to turn Slate positions into viewport pixels). */
	bool ViewportGeometry(APlayerController* PC, FGeometry& Out)
	{
		UWorld* World = PC ? PC->GetWorld() : nullptr;
		UGameViewportClient* GVC = World ? World->GetGameViewport() : nullptr;
		TSharedPtr<SViewport> VP = GVC ? GVC->GetGameViewportWidget() : nullptr;
		if (!VP.IsValid()) { return false; }
		Out = VP->GetPaintSpaceGeometry();
		return true;
	}

	struct FButtonInfo
	{
		UWidget* Widget = nullptr;   // UButton or CommonButtonBase
		UButton* Clickable = nullptr; // the UButton that fires the click (CommonButtonBase: its internal button)
		FString Label;
		FString Owner;
	};

	/** Buttons on this player's screen. A CommonButtonBase is reported once, not with its internal UButton. */
	TArray<FButtonInfo> Buttons(APlayerController* PC)
	{
		TArray<FButtonInfo> Out;
		UClass* CBB = CommonClass(TEXT("CommonButtonBase"));
		TSet<UWidget*> Seen;
		for (TObjectIterator<UButton> It; It; ++It)
		{
			UButton* B = *It;
			if (!IsValid(B) || B->GetWorld() != PC->GetWorld() || !OnScreen(B)) { continue; }
			UUserWidget* OwnerWidget = B->GetTypedOuter<UUserWidget>();
			if (OwnerWidget && OwnerWidget->GetOwningPlayer() && OwnerWidget->GetOwningPlayer() != PC) { continue; }
			FButtonInfo I;
			I.Clickable = B;
			I.Widget = (CBB && OwnerWidget && OwnerWidget->IsA(CBB)) ? (UWidget*)OwnerWidget : (UWidget*)B;
			if (Seen.Contains(I.Widget)) { continue; }
			Seen.Add(I.Widget);
			I.Label = Label(I.Widget);
			UUserWidget* Page = I.Widget->GetTypedOuter<UUserWidget>();
			I.Owner = Page ? Page->GetClass()->GetName() : FString();
			Out.Add(I);
		}
		return Out;
	}

	TArray<TSharedPtr<FJsonValue>> Rect(UWidget* W, const FGeometry& VP)
	{
		const FGeometry G = W->GetCachedWidget()->GetPaintSpaceGeometry();
		const FVector2D TL = VP.AbsoluteToLocal(G.GetAbsolutePosition());
		const FVector2D BR = VP.AbsoluteToLocal(G.GetAbsolutePosition() + G.GetAbsoluteSize());
		return { MakeShared<FJsonValueNumber>(FMath::RoundToDouble(TL.X)), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(TL.Y)),
			MakeShared<FJsonValueNumber>(FMath::RoundToDouble(BR.X)), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(BR.Y)) };
	}

	TSharedRef<FJsonObject> UiState(APlayerController* PC)
	{
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		FGeometry VP;
		const bool bVP = ViewportGeometry(PC, VP);
		UWorld* World = PC->GetWorld();

		TArray<TSharedPtr<FJsonValue>> Screens;
		for (TObjectIterator<UUserWidget> It; It; ++It)
		{
			UUserWidget* U = *It;
			if (!IsValid(U) || U->GetWorld() != World || !U->IsInViewport()) { continue; }
			if (U->GetOwningPlayer() && U->GetOwningPlayer() != PC) { continue; }
			TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetStringField(TEXT("name"), U->GetName());
			J->SetStringField(TEXT("class"), U->GetClass()->GetName());
			J->SetBoolField(TEXT("visible"), OnScreen(U));
			Screens.Add(MakeShared<FJsonValueObject>(J));
		}
		Data->SetArrayField(TEXT("viewportWidgets"), Screens);

		UClass* Activatable = CommonClass(TEXT("CommonActivatableWidget"));
		UClass* Container = CommonClass(TEXT("CommonActivatableWidgetContainerBase"));
		Data->SetBoolField(TEXT("commonUI"), Activatable != nullptr);
		if (Activatable)
		{
			TArray<TSharedPtr<FJsonValue>> Acts;
			for (TObjectIterator<UUserWidget> It; It; ++It)
			{
				UUserWidget* U = *It;
				if (!IsValid(U) || !U->IsA(Activatable) || U->GetWorld() != World) { continue; }
				if (U->GetOwningPlayer() && U->GetOwningPlayer() != PC) { continue; }
				const bool bActive = CallBool(U, TEXT("IsActivated"));
				if (!bActive && !OnScreen(U)) { continue; }
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("name"), U->GetName());
				J->SetStringField(TEXT("class"), U->GetClass()->GetName());
				J->SetBoolField(TEXT("activated"), bActive);
				J->SetBoolField(TEXT("visible"), OnScreen(U));
				Acts.Add(MakeShared<FJsonValueObject>(J));
			}
			Data->SetArrayField(TEXT("activatableWidgets"), Acts);
		}
		const FString InputType = InputTypeName(PC);
		if (!InputType.IsEmpty()) { Data->SetStringField(TEXT("inputType"), InputType); }
		if (Container)
		{
			TArray<TSharedPtr<FJsonValue>> Stacks;
			for (UWidget* C : Containers(PC))
			{
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("name"), C->GetName());
				J->SetStringField(TEXT("class"), C->GetClass()->GetName());
				if (UUserWidget* Owner = C->GetTypedOuter<UUserWidget>()) { J->SetStringField(TEXT("owner"), Owner->GetClass()->GetName()); }
				TArray<TSharedPtr<FJsonValue>> List;
				for (UObject* W : StackList(C)) { List.Add(MakeShared<FJsonValueString>(W ? W->GetClass()->GetName() : TEXT("null"))); }
				J->SetArrayField(TEXT("widgets"), List);
				if (List.Num()) { J->SetStringField(TEXT("top"), List.Last()->AsString()); }
				Stacks.Add(MakeShared<FJsonValueObject>(J));
			}
			Data->SetArrayField(TEXT("stacks"), Stacks);
		}

		TArray<TSharedPtr<FJsonValue>> Btns;
		for (const FButtonInfo& B : Buttons(PC))
		{
			TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetStringField(TEXT("name"), B.Widget->GetName());
			J->SetStringField(TEXT("class"), B.Widget->GetClass()->GetName());
			J->SetStringField(TEXT("label"), B.Label);
			J->SetStringField(TEXT("page"), B.Owner);
			J->SetBoolField(TEXT("enabled"), B.Widget->GetIsEnabled() && B.Clickable->GetIsEnabled());
			if (B.Widget != B.Clickable && B.Widget->FindFunction(TEXT("GetSelected"))) { J->SetBoolField(TEXT("selected"), CallBool(B.Widget, TEXT("GetSelected"))); }
			if (B.Widget != B.Clickable && B.Widget->FindFunction(TEXT("IsInteractionEnabled"))) { J->SetBoolField(TEXT("interactionEnabled"), CallBool(B.Widget, TEXT("IsInteractionEnabled"))); }
			J->SetStringField(TEXT("path"), B.Widget->GetPathName());
			if (bVP) { J->SetArrayField(TEXT("rect"), Rect(B.Widget, VP)); }
			Btns.Add(MakeShared<FJsonValueObject>(J));
		}
		Data->SetArrayField(TEXT("buttons"), Btns);
		return Data;
	}
}

void RegisterUIRuntimeTools(FEWRegistry& R)
{
	R.Register(TEXT("ui_state"),
		TEXT("What a Play In Editor player's UI shows: widgets in the viewport, CommonUI activatable widgets (activated or not) and activatable stacks with their widget list and top, and every visible button (UMG Button or CommonButtonBase) with label, page, enabled and its rectangle in viewport pixels."),
		TEXT(R"({"type":"object","properties":{"player":{"type":"integer","description":"Local player index, default 0."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Err;
			APlayerController* PC = EW::PiePlayer((int32)EW::Num(Args, TEXT("player"), 0), Err);
			if (!PC) { return Bad(TEXT("NO_PLAYER"), Err); }
			return FEWResult::Ok(UiState(PC));
		});

	R.RegisterAsync(TEXT("ui_click"),
		TEXT("Click a visible button in Play In Editor, found by widget name or label (wildcards allowed). method click (default) first checks that no other hit-testable widget is drawn over the button center (else COVERED), then presses the Slate button (press + click, as a pointer or gamepad accept does); method event fires the UMG OnClicked directly, even when covered. Returns the UI state after settleSeconds."),
		TEXT(R"({"type":"object","properties":{
			"player":{"type":"integer","description":"Play In Editor player index (0 = first, default 0)."},
			"button":{"type":"string","description":"Widget name or label, e.g. Play or *Settings*."},
			"method":{"type":"string","enum":["click","event"],"description":"click (default): checked like a real click, refused when covered; event: fire OnClicked directly."},
			"settleSeconds":{"type":"number","description":"Wait after the click before reading the UI, default 0.3."}},
			"required":["button"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args, FEWDone Done)
		{
			FString Err;
			APlayerController* PC = EW::PiePlayer((int32)EW::Num(Args, TEXT("player"), 0), Err);
			if (!PC) { Done(Bad(TEXT("NO_PLAYER"), Err)); return; }
			const FString Key = EW::Str(Args, TEXT("button"));
			const FString Method = Args->HasField(TEXT("method")) ? EW::Str(Args, TEXT("method")).ToLower() : TEXT("click");
			if (Method != TEXT("click") && Method != TEXT("event")) { Done(Bad(TEXT("INVALID_ARGUMENT"), TEXT("method must be click or event."))); return; }
			const float Settle = FMath::Clamp((float)EW::Num(Args, TEXT("settleSeconds"), 0.3), 0.f, 10.f);

			const TArray<FButtonInfo> All = Buttons(PC);
			TArray<FButtonInfo> Hits;
			TArray<FString> Names;
			for (const FButtonInfo& B : All)
			{
				Names.Add(B.Label.IsEmpty() ? B.Widget->GetName() : B.Label);
				if (B.Widget->GetName() == Key || B.Label.Equals(Key, ESearchCase::IgnoreCase) || B.Label.MatchesWildcard(Key) || B.Widget->GetName().MatchesWildcard(Key)) { Hits.Add(B); }
			}
			if (Hits.Num() == 0) { Done(Bad(TEXT("BUTTON_NOT_FOUND"), FString::Printf(TEXT("No visible button '%s' on player %d's screen."), *Key, (int32)EW::Num(Args, TEXT("player"), 0)), EW::Suggest(Key, Names))); return; }
			if (Hits.Num() > 1)
			{
				TArray<FString> Which;
				for (const FButtonInfo& B : Hits) { Which.Add(B.Widget->GetName()); }
				Done(Bad(TEXT("AMBIGUOUS"), FString::Printf(TEXT("%d buttons match '%s'; pass the widget name."), Hits.Num(), *Key), Which));
				return;
			}
			const FButtonInfo B = Hits[0];
			if (!B.Widget->GetIsEnabled() || !B.Clickable->GetIsEnabled()) { Done(Bad(TEXT("BUTTON_DISABLED"), FString::Printf(TEXT("'%s' is disabled."), *B.Widget->GetName()))); return; }

			TSharedRef<FJsonObject> Clicked = MakeShared<FJsonObject>();
			Clicked->SetStringField(TEXT("name"), B.Widget->GetName());
			Clicked->SetStringField(TEXT("label"), B.Label);
			Clicked->SetStringField(TEXT("method"), Method);
			TWeakObjectPtr<APlayerController> WeakPC = PC;
			auto Finish = [Done, Clicked, WeakPC, Settle]()
			{
				EW::PollUntil(Settle, []() { return false; }, [Done, Clicked, WeakPC]()
				{
					if (!WeakPC.IsValid()) { Done(Bad(TEXT("NO_PLAYER"), TEXT("The player went away after the click."))); return; }
					TSharedRef<FJsonObject> Data = UiState(WeakPC.Get());
					Data->SetObjectField(TEXT("clicked"), Clicked);
					Done(FEWResult::Ok(Data));
				});
			};

			if (Method == TEXT("event"))
			{
				B.Clickable->OnClicked.Broadcast();
				Finish();
				return;
			}

			TSharedPtr<SWidget> Slate = B.Widget->GetCachedWidget();
			const FGeometry G = Slate->GetPaintSpaceGeometry();
			const FVector2D Center = G.GetAbsolutePosition() + G.GetAbsoluteSize() * 0.5f;
			if (UWidget* Over = Blocker(PC->GetWorld(), B.Widget, Center))
			{
				UUserWidget* OverPage = Over->GetTypedOuter<UUserWidget>();
				Done(Bad(TEXT("COVERED"), FString::Printf(TEXT("'%s' is covered by %s (%s%s) at its center. Use method event to fire it anyway."), *B.Widget->GetName(), *Over->GetName(), *Over->GetClass()->GetName(), OverPage ? *(TEXT(" in ") + OverPage->GetClass()->GetName()) : TEXT(""))));
				return;
			}
			// Press the Slate button itself (press, then click: the same path as a pointer or gamepad accept).
			TSharedPtr<SButton> Btn = FindSButton(B.Clickable->GetCachedWidget());
			if (!Btn.IsValid()) { Btn = FindSButton(Slate); }
			if (!Btn.IsValid()) { Done(Bad(TEXT("CLICK_FAILED"), FString::Printf(TEXT("'%s' has no Slate button to press."), *B.Widget->GetName()))); return; }
			Btn->SimulateClick();
			Finish();
		});

	R.Register(TEXT("ui_open"),
		TEXT("Open a widget class (Widget Blueprint or C++ UserWidget) on a Play In Editor player's screen: creates it for that player and adds it to the viewport; a CommonUI activatable widget is also activated. With stack (name of a running CommonActivatableWidgetStack / Queue, see ui_state stacks) the activatable widget is pushed onto that stack instead, as the game's own UI flow would do. Use it to test one screen in isolation."),
		TEXT(R"({"type":"object","properties":{
			"class":{"type":"string","description":"Widget Blueprint path or class name."},
			"player":{"type":"integer","description":"Play In Editor player index (0 = first, default 0)."},
			"stack":{"type":"string","description":"Optional: push onto this activatable widget stack (widget name or owner class name)."},
			"zOrder":{"type":"integer","description":"Default 10."}},
			"required":["class"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Err;
			APlayerController* PC = EW::PiePlayer((int32)EW::Num(Args, TEXT("player"), 0), Err);
			if (!PC) { return Bad(TEXT("NO_PLAYER"), Err); }
			TArray<FString> Sug;
			UClass* C = EW::FindClass(EW::Str(Args, TEXT("class")), Err, &Sug);
			if (!C) { return Bad(TEXT("UNKNOWN_CLASS"), Err, Sug); }
			if (!C->IsChildOf(UUserWidget::StaticClass()) || C->HasAnyClassFlags(CLASS_Abstract)) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("%s is not a concrete UserWidget class."), *C->GetName())); }
			UClass* Act = CommonClass(TEXT("CommonActivatableWidget"));

			const FString StackKey = EW::Str(Args, TEXT("stack"));
			if (!StackKey.IsEmpty())
			{
				if (!Act) { return Bad(TEXT("NO_COMMONUI"), TEXT("stack needs the CommonUI plugin, which is not loaded.")); }
				if (!C->IsChildOf(Act)) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("%s is not a CommonActivatableWidget; only those can be pushed onto a stack."), *C->GetName())); }
				TArray<UWidget*> Matches;
				TArray<FString> Names;
				for (UWidget* S : Containers(PC))
				{
					UUserWidget* Owner = S->GetTypedOuter<UUserWidget>();
					const FString OwnerClass = Owner ? Owner->GetClass()->GetName() : FString();
					Names.Add(S->GetName());
					if (S->GetName() == StackKey || S->GetName().MatchesWildcard(StackKey) || OwnerClass == StackKey || OwnerClass == StackKey + TEXT("_C")) { Matches.Add(S); }
				}
				if (Matches.Num() == 0) { return Bad(TEXT("STACK_NOT_FOUND"), FString::Printf(TEXT("No running activatable widget stack '%s' for this player."), *StackKey), Names.Num() ? EW::Suggest(StackKey, Names) : TArray<FString>{ TEXT("ui_state lists stacks; open the screen that owns the stack first.") }); }
				if (Matches.Num() > 1) { TArray<FString> Which; for (UWidget* S : Matches) { Which.Add(S->GetName()); } return Bad(TEXT("AMBIGUOUS"), FString::Printf(TEXT("%d stacks match '%s'; pass the widget name."), Matches.Num(), *StackKey), Which); }
				UWidget* Stack = Matches[0];
				const int32 Before = StackList(Stack).Num();
				bool bCalled = false;
				UObject* Pushed = CallWithObject(Stack, TEXT("BP_AddWidget"), C, &bCalled);
				if (!bCalled) { return Bad(TEXT("PUSH_FAILED"), FString::Printf(TEXT("%s has no BP_AddWidget (unsupported CommonUI version)."), *Stack->GetClass()->GetName())); }
				const TArray<UObject*> After = StackList(Stack);
				if (!Pushed || !After.Contains(Pushed)) { return Bad(TEXT("PUSH_FAILED"), FString::Printf(TEXT("%s did not take the widget (stack had %d, now %d)."), *Stack->GetName(), Before, After.Num())); }
				TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("name"), Pushed->GetName());
				Data->SetStringField(TEXT("class"), C->GetName());
				Data->SetStringField(TEXT("stack"), Stack->GetName());
				Data->SetNumberField(TEXT("stackSize"), After.Num());
				Data->SetBoolField(TEXT("activated"), CallBool(Pushed, TEXT("IsActivated")));
				return FEWResult::Ok(Data);
			}

			UUserWidget* W = CreateWidget<UUserWidget>(PC, TSubclassOf<UUserWidget>(C));
			if (!W) { return Bad(TEXT("CREATE_FAILED"), TEXT("Could not create the widget.")); }
			W->AddToViewport((int32)EW::Num(Args, TEXT("zOrder"), 10));
			bool bActivated = false;
			if (Act)
			{
				if (W->IsA(Act))
				{
					if (UFunction* F = W->FindFunction(TEXT("ActivateWidget"))) { W->ProcessEvent(F, nullptr); }
					bActivated = CallBool(W, TEXT("IsActivated"));
				}
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("name"), W->GetName());
			Data->SetStringField(TEXT("class"), C->GetName());
			Data->SetBoolField(TEXT("inViewport"), W->IsInViewport());
			Data->SetBoolField(TEXT("activated"), bActivated);
			if (!W->IsInViewport()) { return Bad(TEXT("OPEN_FAILED"), TEXT("The widget was created but is not in the viewport.")); }
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("ui_close"),
		TEXT("Remove a widget from a Play In Editor player's screen by widget name (as listed by ui_state viewportWidgets) or class name."),
		TEXT(R"({"type":"object","properties":{"widget":{"type":"string","description":"Widget name or class name (wildcards allowed), as listed by ui_state."},"player":{"type":"integer","description":"Play In Editor player index (0 = first, default 0)."}},"required":["widget"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Err;
			APlayerController* PC = EW::PiePlayer((int32)EW::Num(Args, TEXT("player"), 0), Err);
			if (!PC) { return Bad(TEXT("NO_PLAYER"), Err); }
			const FString Key = EW::Str(Args, TEXT("widget"));
			TArray<FString> Names, Closed;
			for (TObjectIterator<UUserWidget> It; It; ++It)
			{
				UUserWidget* U = *It;
				if (!IsValid(U) || U->GetWorld() != PC->GetWorld() || !U->IsInViewport()) { continue; }
				if (U->GetOwningPlayer() && U->GetOwningPlayer() != PC) { continue; }
				Names.Add(U->GetName());
				if (U->GetName() == Key || U->GetClass()->GetName() == Key || U->GetClass()->GetName() == Key + TEXT("_C"))
				{
					if (UClass* Act = CommonClass(TEXT("CommonActivatableWidget")))
					{
						if (U->IsA(Act)) { if (UFunction* F = U->FindFunction(TEXT("DeactivateWidget"))) { U->ProcessEvent(F, nullptr); } }
					}
					U->RemoveFromParent();
					Closed.Add(U->GetName());
				}
			}
			// Widgets inside an activatable stack: remove them from their stack (the stack deactivates them).
			for (UWidget* S : Containers(PC))
			{
				for (UObject* W : StackList(S))
				{
					if (!W) { continue; }
					Names.Add(W->GetName());
					if (W->GetName() == Key || W->GetClass()->GetName() == Key || W->GetClass()->GetName() == Key + TEXT("_C"))
					{
						CallWithObject(S, TEXT("RemoveWidget"), W);
						// The top widget leaves the list after the stack's transition; it is deactivated right away.
						if (!StackList(S).Contains(W) || !CallBool(W, TEXT("IsActivated"))) { Closed.Add(W->GetName()); }
						else { return Bad(TEXT("CLOSE_FAILED"), FString::Printf(TEXT("%s is still active on %s after RemoveWidget."), *W->GetName(), *S->GetName())); }
					}
				}
			}
			if (Closed.Num() == 0) { return Bad(TEXT("WIDGET_NOT_FOUND"), FString::Printf(TEXT("No widget '%s' in player's viewport."), *Key), EW::Suggest(Key, Names)); }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> J;
			for (const FString& N : Closed) { J.Add(MakeShared<FJsonValueString>(N)); }
			Data->SetArrayField(TEXT("closed"), J);
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("ui_input_type"),
		TEXT("Read or switch a Play In Editor player's CommonUI input type (MouseAndKeyboard, Gamepad, Touch), as if the player picked up a gamepad. Use it to check gamepad glyphs, focus and button hints without a controller. Needs the CommonUI plugin."),
		TEXT(R"({"type":"object","properties":{
			"player":{"type":"integer","description":"Play In Editor player index (0 = first, default 0)."},
			"type":{"type":"string","enum":["MouseAndKeyboard","Gamepad","Touch"],"description":"Omit to only read the current type."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Err;
			APlayerController* PC = EW::PiePlayer((int32)EW::Num(Args, TEXT("player"), 0), Err);
			if (!PC) { return Bad(TEXT("NO_PLAYER"), Err); }
			UObject* Sub = CommonInputSubsystem(PC);
			UEnum* E = CommonInputTypeEnum();
			if (!Sub || !E) { return Bad(TEXT("NO_COMMONUI"), TEXT("The CommonUI plugin (CommonInput subsystem) is not active for this player.")); }
			const FString Before = InputTypeName(PC);
			const FString Want = EW::Str(Args, TEXT("type"));
			if (!Want.IsEmpty())
			{
				const int64 V = E->GetValueByNameString(Want);
				if (V == INDEX_NONE || Want == TEXT("Count") || Want == TEXT("MAX")) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Unknown input type '%s'."), *Want), { TEXT("MouseAndKeyboard"), TEXT("Gamepad"), TEXT("Touch") }); }
				UFunction* F = Sub->FindFunction(TEXT("SetCurrentInputType"));
				if (!F) { return Bad(TEXT("UNSUPPORTED"), TEXT("CommonInputSubsystem has no SetCurrentInputType.")); }
				FStructOnScope Parms(F);
				for (TFieldIterator<FProperty> It(F); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
				{
					void* Ptr = It->ContainerPtrToValuePtr<void>(Parms.GetStructMemory());
					if (FEnumProperty* EP = CastField<FEnumProperty>(*It)) { EP->GetUnderlyingProperty()->SetIntPropertyValue(Ptr, V); }
					else if (FNumericProperty* NP = CastField<FNumericProperty>(*It)) { NP->SetIntPropertyValue(Ptr, V); }
					break;
				}
				Sub->ProcessEvent(F, Parms.GetStructMemory());
			}
			const FString Now = InputTypeName(PC);
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("inputType"), Now);
			if (!Want.IsEmpty())
			{
				Data->SetStringField(TEXT("before"), Before);
				if (Now != Want)
				{
					// CommonInput refuses types the platform does not support (see CommonInputSettings / platform traits).
					return Bad(TEXT("NOT_APPLIED"), FString::Printf(TEXT("Input type is still %s after asking for %s; this platform's CommonInput settings may not allow it."), *Now, *Want));
				}
			}
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("ui_input_audit"),
		TEXT("Check CommonUI input wiring in Widget Blueprints (read-only): CommonButtonBase triggering input actions whose DataTable row does not exist, Enhanced Input actions on buttons while CommonInput's Enhanced Input support is off (they never fire), two buttons in one widget triggering the same action, and a missing CommonUI input data asset (no default click / back action). Returns issues with asset, widget and a fix hint."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string","description":"Content folder to scan, default /Game."},
			"maxAssets":{"type":"integer","description":"Default 500."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			UClass* CBB = CommonClass(TEXT("CommonButtonBase"));
			if (!CBB) { return Bad(TEXT("NO_COMMONUI"), TEXT("The CommonUI plugin is not loaded; nothing to check.")); }
			FString Folder = EW::Str(Args, TEXT("path"));
			if (Folder.IsEmpty()) { Folder = TEXT("/Game"); }
			if (!Folder.StartsWith(TEXT("/"))) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("path must be a content folder like /Game/UI.")); }
			const int32 MaxAssets = FMath::Clamp((int32)EW::Num(Args, TEXT("maxAssets"), 500), 1, 5000);

			bool bEnhanced = false, bInputData = false;
			if (UClass* SettingsClass = FindObject<UClass>(nullptr, TEXT("/Script/CommonInput.CommonInputSettings")))
			{
				UObject* S = SettingsClass->GetDefaultObject();
				if (const FBoolProperty* P = CastField<FBoolProperty>(SettingsClass->FindPropertyByName(TEXT("bEnableEnhancedInputSupport")))) { bEnhanced = P->GetPropertyValue_InContainer(S); }
				if (const FSoftClassProperty* P = CastField<FSoftClassProperty>(SettingsClass->FindPropertyByName(TEXT("InputData")))) { bInputData = !P->GetPropertyValue_InContainer(S).IsNull(); }
			}

			TArray<TSharedPtr<FJsonValue>> Issues;
			auto Issue = [&Issues](const FString& Code, const FString& Asset, const FString& Widget, const FString& Msg, const FString& Fix)
			{
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("code"), Code);
				J->SetStringField(TEXT("asset"), Asset);
				if (!Widget.IsEmpty()) { J->SetStringField(TEXT("widget"), Widget); }
				J->SetStringField(TEXT("message"), Msg);
				J->SetStringField(TEXT("fix"), Fix);
				Issues.Add(MakeShared<FJsonValueObject>(J));
			};

			const FStructProperty* RowProp = CastField<FStructProperty>(CBB->FindPropertyByName(TEXT("TriggeringInputAction")));
			const FObjectPropertyBase* IAProp = CastField<FObjectPropertyBase>(CBB->FindPropertyByName(TEXT("TriggeringEnhancedInputAction")));
			int32 Scanned = 0, ButtonCount = 0, WithAction = 0;
			TArray<FString> Assets = EW::FindAssets(Folder, UWidgetBlueprint::StaticClass());
			const bool bTruncated = Assets.Num() > MaxAssets;
			if (bTruncated) { Assets.SetNum(MaxAssets); }
			for (const FString& Path : Assets)
			{
				FString Err;
				UWidgetBlueprint* WBP = EW::LoadAs<UWidgetBlueprint>(Path, Err);
				if (!WBP || !WBP->GeneratedClass) { continue; }
				++Scanned;
				// The button templates placed in this widget, plus the class defaults when the Blueprint itself is a button.
				TArray<TPair<FString, UObject*>> Candidates;
				if (WBP->GeneratedClass->IsChildOf(CBB)) { Candidates.Emplace(TEXT("(class defaults)"), WBP->GeneratedClass->GetDefaultObject()); }
				if (WBP->WidgetTree)
				{
					TArray<UWidget*> All;
					WBP->WidgetTree->GetAllWidgets(All);
					for (UWidget* W : All) { if (W && W->IsA(CBB)) { Candidates.Emplace(W->GetName(), W); } }
				}
				TMap<FString, FString> SeenAction; // action key -> first widget
				for (const TPair<FString, UObject*>& C : Candidates)
				{
					++ButtonCount;
					FString ActionKey;
					if (RowProp && RowProp->Struct == FDataTableRowHandle::StaticStruct())
					{
						const FDataTableRowHandle* H = RowProp->ContainerPtrToValuePtr<FDataTableRowHandle>(C.Value);
						if (!H->RowName.IsNone() || H->DataTable)
						{
							ActionKey = FString::Printf(TEXT("%s:%s"), H->DataTable ? *H->DataTable->GetName() : TEXT("None"), *H->RowName.ToString());
							if (!H->DataTable) { Issue(TEXT("ROW_WITHOUT_TABLE"), Path, C.Key, FString::Printf(TEXT("TriggeringInputAction names row '%s' but no DataTable."), *H->RowName.ToString()), TEXT("Pick the input action DataTable or clear the row.")); }
							else if (H->RowName.IsNone() || !H->DataTable->GetRowMap().Contains(H->RowName))
							{
								TArray<FString> Rows; for (const FName& N : H->DataTable->GetRowNames()) { Rows.Add(N.ToString()); }
								const TArray<FString> Near = EW::Suggest(H->RowName.ToString(), Rows);
								Issue(TEXT("ROW_MISSING"), Path, C.Key, FString::Printf(TEXT("TriggeringInputAction row '%s' is not in %s."), *H->RowName.ToString(), *H->DataTable->GetPathName()),
									Near.Num() ? FString::Printf(TEXT("Did you mean %s?"), *FString::Join(Near, TEXT(", "))) : TEXT("Add the row or pick an existing one."));
							}
						}
					}
					if (IAProp)
					{
						if (UObject* IA = IAProp->GetObjectPropertyValue_InContainer(C.Value))
						{
							ActionKey = IA->GetPathName();
							if (!bEnhanced) { Issue(TEXT("ENHANCED_INPUT_OFF"), Path, C.Key, FString::Printf(TEXT("TriggeringEnhancedInputAction is %s but CommonInput Enhanced Input support is off, so it never triggers."), *IA->GetName()), TEXT("Turn on Project Settings > Common Input > Enable Enhanced Input Support, or use TriggeringInputAction (DataTable row).")); }
						}
					}
					if (ActionKey.IsEmpty()) { continue; }
					++WithAction;
					if (const FString* First = SeenAction.Find(ActionKey))
					{
						if (*First != C.Key) { Issue(TEXT("DUPLICATE_ACTION"), Path, C.Key, FString::Printf(TEXT("Triggers the same input action as %s (%s); only one of them will react."), **First, *ActionKey), TEXT("Give each button its own action, or show only one at a time.")); }
					}
					else { SeenAction.Add(ActionKey, C.Key); }
				}
			}
			if (ButtonCount > 0 && !bInputData) { Issue(TEXT("NO_INPUT_DATA"), TEXT("Project Settings > Common Input"), FString(), TEXT("CommonInputSettings.InputData is not set: CommonUI has no default click / back action, so gamepad accept and back do nothing on CommonUI buttons and pages."), TEXT("Create a CommonUIInputData Blueprint with click and back actions and set it in Project Settings > Common Input > Input Data.")); }

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("path"), Folder);
			Data->SetNumberField(TEXT("widgetBlueprints"), Scanned);
			Data->SetNumberField(TEXT("commonButtons"), ButtonCount);
			Data->SetNumberField(TEXT("buttonsWithAction"), WithAction);
			Data->SetBoolField(TEXT("enhancedInputSupport"), bEnhanced);
			Data->SetBoolField(TEXT("inputDataSet"), bInputData);
			Data->SetBoolField(TEXT("truncated"), bTruncated);
			Data->SetArrayField(TEXT("issues"), Issues);
			Data->SetNumberField(TEXT("issueCount"), Issues.Num());
			return FEWResult::Ok(Data);
		});
}
