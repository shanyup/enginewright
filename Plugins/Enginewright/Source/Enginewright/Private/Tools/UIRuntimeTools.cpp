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
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
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
		if (Container)
		{
			TArray<TSharedPtr<FJsonValue>> Stacks;
			for (TObjectIterator<UWidget> It; It; ++It)
			{
				UWidget* C = *It;
				if (!IsValid(C) || !C->IsA(Container) || C->GetWorld() != World || !C->GetCachedWidget().IsValid()) { continue; }
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("name"), C->GetName());
				J->SetStringField(TEXT("class"), C->GetClass()->GetName());
				if (UUserWidget* Owner = C->GetTypedOuter<UUserWidget>()) { J->SetStringField(TEXT("owner"), Owner->GetClass()->GetName()); }
				TArray<TSharedPtr<FJsonValue>> List;
				if (const FArrayProperty* AP = CastField<FArrayProperty>(C->GetClass()->FindPropertyByName(TEXT("WidgetList"))))
				{
					FScriptArrayHelper H(AP, AP->ContainerPtrToValuePtr<void>(C));
					const FObjectPropertyBase* Inner = CastField<FObjectPropertyBase>(AP->Inner);
					for (int32 i = 0; Inner && i < H.Num(); ++i)
					{
						UObject* W = Inner->GetObjectPropertyValue(H.GetRawPtr(i));
						List.Add(MakeShared<FJsonValueString>(W ? W->GetClass()->GetName() : TEXT("null")));
					}
				}
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
			"player":{"type":"integer"},
			"button":{"type":"string","description":"Widget name or label, e.g. Play or *Settings*."},
			"method":{"type":"string","enum":["click","event"]},
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
		TEXT("Open a widget class (Widget Blueprint or C++ UserWidget) on a Play In Editor player's screen: creates it for that player and adds it to the viewport; a CommonUI activatable widget is also activated. Use it to test one screen in isolation."),
		TEXT(R"({"type":"object","properties":{
			"class":{"type":"string","description":"Widget Blueprint path or class name."},
			"player":{"type":"integer"},
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
			UUserWidget* W = CreateWidget<UUserWidget>(PC, TSubclassOf<UUserWidget>(C));
			if (!W) { return Bad(TEXT("CREATE_FAILED"), TEXT("Could not create the widget.")); }
			W->AddToViewport((int32)EW::Num(Args, TEXT("zOrder"), 10));
			bool bActivated = false;
			if (UClass* Act = CommonClass(TEXT("CommonActivatableWidget")))
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
		TEXT(R"({"type":"object","properties":{"widget":{"type":"string"},"player":{"type":"integer"}},"required":["widget"]})"),
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
			if (Closed.Num() == 0) { return Bad(TEXT("WIDGET_NOT_FOUND"), FString::Printf(TEXT("No widget '%s' in player's viewport."), *Key), EW::Suggest(Key, Names)); }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> J;
			for (const FString& N : Closed) { J.Add(MakeShared<FJsonValueString>(N)); }
			Data->SetArrayField(TEXT("closed"), J);
			return FEWResult::Ok(Data);
		});
}
