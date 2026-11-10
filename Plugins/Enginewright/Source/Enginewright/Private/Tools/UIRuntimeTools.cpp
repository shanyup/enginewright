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
#include "InputCoreTypes.h"
#include "Framework/Application/SlateUser.h"
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
		FString Layer = TEXT("viewport"); // the activatable stack that holds the page, or "viewport" for a plain widget
		bool bTop = true;                 // false: the page is deactivated or sits below another page of its stack
		FString HiddenReason;             // why it is not top (for includeHidden and error messages)
	};

	/**
	 * Where a widget sits: Layer = the CommonUI stack that holds its page ("viewport" for a plain UMG widget);
	 * bTop = false when a page above it is not activated, or another page of the same stack is drawn over it.
	 */
	void Classify(UWidget* W, const TArray<UWidget*>& Stacks, FString& Layer, bool& bTop, FString& Reason)
	{
		Layer = TEXT("viewport");
		bTop = true;
		UClass* Act = CommonClass(TEXT("CommonActivatableWidget"));
		if (!Act) { return; }
		for (UObject* O = W; O; O = O->GetOuter())
		{
			if (!O->IsA(Act)) { continue; }
			if (!CallBool(O, TEXT("IsActivated")))
			{
				bTop = false;
				if (Reason.IsEmpty()) { Reason = FString::Printf(TEXT("page %s is not activated"), *O->GetClass()->GetName()); }
			}
			for (UWidget* S : Stacks)
			{
				const TArray<UObject*> List = StackList(S);
				if (!List.Contains(O)) { continue; }
				Layer = S->GetName();
				if (List.Last() != O)
				{
					bTop = false;
					if (Reason.IsEmpty()) { Reason = FString::Printf(TEXT("page %s is below %s on stack %s"), *O->GetClass()->GetName(), List.Last() ? *List.Last()->GetClass()->GetName() : TEXT("?"), *S->GetName()); }
				}
			}
		}
	}

	/** Buttons on this player's screen. A CommonButtonBase is reported once, not with its internal UButton.
	 *  Off-screen buttons are skipped unless bIncludeOffscreen. */
	TArray<FButtonInfo> Buttons(APlayerController* PC, bool bIncludeOffscreen = false)
	{
		TArray<FButtonInfo> Out;
		UClass* CBB = CommonClass(TEXT("CommonButtonBase"));
		const TArray<UWidget*> Stacks = Containers(PC);
		TSet<UWidget*> Seen;
		for (TObjectIterator<UButton> It; It; ++It)
		{
			UButton* B = *It;
			if (!IsValid(B) || B->GetWorld() != PC->GetWorld() || (!bIncludeOffscreen && !OnScreen(B))) { continue; }
			if (bIncludeOffscreen && !B->GetCachedWidget().IsValid()) { continue; }
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
			Classify(I.Widget, Stacks, I.Layer, I.bTop, I.HiddenReason);
			if (!OnScreen(I.Widget) && I.HiddenReason.IsEmpty()) { I.HiddenReason = TEXT("not drawn (hidden, collapsed or zero size)"); }
			Out.Add(I);
		}
		return Out;
	}

	/** Visible and on the top page of its stack: what the player can use right now. */
	bool IsUsable(const FButtonInfo& B) { return B.bTop && OnScreen(B.Widget); }

	bool ParseKeyName(const FString& Name, FKey& Out)
	{
		Out = FKey(FName(*Name));
		if (Out.IsValid()) { return true; }
		TArray<FKey> All;
		EKeys::GetAllKeys(All);
		for (const FKey& K : All) { if (K.GetFName().ToString().Equals(Name, ESearchCase::IgnoreCase)) { Out = K; return true; } }
		return false;
	}

	TArray<FString> SuggestKeys(const FString& Name)
	{
		TArray<FKey> All;
		EKeys::GetAllKeys(All);
		TArray<FString> Names;
		for (const FKey& K : All) { Names.Add(K.GetFName().ToString()); }
		return EW::Suggest(Name, Names);
	}

	/** What has user focus: the Slate widget type and the UMG widget (and page) that owns it. */
	TSharedRef<FJsonObject> FocusInfo(APlayerController* PC, int32 UserIndex)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		TSharedPtr<SWidget> F = FSlateApplication::IsInitialized() ? FSlateApplication::Get().GetUserFocusedWidget(UserIndex) : nullptr;
		J->SetBoolField(TEXT("focused"), F.IsValid());
		J->SetStringField(TEXT("slateType"), F.IsValid() ? F->GetType().ToString() : TEXT("none"));
		if (!F.IsValid()) { return J; }
		TMap<const SWidget*, UWidget*> Owners;
		for (TObjectIterator<UWidget> It; It; ++It)
		{
			UWidget* W = *It;
			if (IsValid(W) && W->GetWorld() == PC->GetWorld() && W->GetCachedWidget().IsValid()) { Owners.Add(W->GetCachedWidget().Get(), W); }
		}
		for (TSharedPtr<SWidget> P = F; P.IsValid(); P = P->GetParentWidget())
		{
			if (UWidget* const* W = Owners.Find(P.Get()))
			{
				J->SetStringField(TEXT("widget"), (*W)->GetName());
				J->SetStringField(TEXT("class"), (*W)->GetClass()->GetName());
				UUserWidget* Page = (*W)->GetTypedOuter<UUserWidget>();
				if (UUserWidget* Self = Cast<UUserWidget>(*W)) { Page = Self; }
				if (Page) { J->SetStringField(TEXT("page"), Page->GetClass()->GetName()); }
				break;
			}
		}
		return J;
	}

	TArray<TSharedPtr<FJsonValue>> Rect(UWidget* W, const FGeometry& VP)
	{
		const FGeometry G = W->GetCachedWidget()->GetPaintSpaceGeometry();
		const FVector2D TL = VP.AbsoluteToLocal(G.GetAbsolutePosition());
		const FVector2D BR = VP.AbsoluteToLocal(G.GetAbsolutePosition() + G.GetAbsoluteSize());
		return { MakeShared<FJsonValueNumber>(FMath::RoundToDouble(TL.X)), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(TL.Y)),
			MakeShared<FJsonValueNumber>(FMath::RoundToDouble(BR.X)), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(BR.Y)) };
	}

	/** Lists only what the player can use now (visible, top page); bIncludeHidden adds the rest with the reason. */
	TSharedRef<FJsonObject> UiState(APlayerController* PC, bool bIncludeHidden = false)
	{
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		FGeometry VP;
		const bool bVP = ViewportGeometry(PC, VP);
		UWorld* World = PC->GetWorld();

		TArray<TSharedPtr<FJsonValue>> Screens;
		int32 HiddenWidgets = 0, HiddenButtons = 0;
		const TArray<UWidget*> StackWidgets = Containers(PC);
		for (TObjectIterator<UUserWidget> It; It; ++It)
		{
			UUserWidget* U = *It;
			if (!IsValid(U) || U->GetWorld() != World || !U->IsInViewport()) { continue; }
			if (U->GetOwningPlayer() && U->GetOwningPlayer() != PC) { continue; }
			TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetStringField(TEXT("name"), U->GetName());
			J->SetStringField(TEXT("class"), U->GetClass()->GetName());
			const bool bVisible = OnScreen(U);
			J->SetBoolField(TEXT("visible"), bVisible);
			if (!bVisible && !bIncludeHidden) { ++HiddenWidgets; continue; }
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
				const bool bVisible = OnScreen(U);
				if (!bActive && !bVisible) { continue; }
				FString Layer, Reason;
				bool bTop = true;
				Classify(U, StackWidgets, Layer, bTop, Reason);
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("name"), U->GetName());
				J->SetStringField(TEXT("class"), U->GetClass()->GetName());
				J->SetBoolField(TEXT("activated"), bActive);
				J->SetBoolField(TEXT("visible"), bVisible);
				J->SetStringField(TEXT("layer"), Layer);
				J->SetBoolField(TEXT("top"), bTop);
				if (!Reason.IsEmpty()) { J->SetStringField(TEXT("hiddenReason"), Reason); }
				if (!bIncludeHidden && !(bActive && bVisible && bTop)) { ++HiddenWidgets; continue; }
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
		for (const FButtonInfo& B : Buttons(PC, bIncludeHidden))
		{
			if (!IsUsable(B) && !bIncludeHidden) { ++HiddenButtons; continue; }
			TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetStringField(TEXT("name"), B.Widget->GetName());
			J->SetStringField(TEXT("class"), B.Widget->GetClass()->GetName());
			J->SetStringField(TEXT("label"), B.Label);
			J->SetStringField(TEXT("page"), B.Owner);
			J->SetStringField(TEXT("layer"), B.Layer);
			J->SetBoolField(TEXT("top"), B.bTop);
			J->SetBoolField(TEXT("visible"), OnScreen(B.Widget));
			if (!B.HiddenReason.IsEmpty()) { J->SetStringField(TEXT("hiddenReason"), B.HiddenReason); }
			J->SetBoolField(TEXT("enabled"), B.Widget->GetIsEnabled() && B.Clickable->GetIsEnabled());
			if (B.Widget != B.Clickable && B.Widget->FindFunction(TEXT("GetSelected"))) { J->SetBoolField(TEXT("selected"), CallBool(B.Widget, TEXT("GetSelected"))); }
			if (B.Widget != B.Clickable && B.Widget->FindFunction(TEXT("IsInteractionEnabled"))) { J->SetBoolField(TEXT("interactionEnabled"), CallBool(B.Widget, TEXT("IsInteractionEnabled"))); }
			J->SetStringField(TEXT("path"), B.Widget->GetPathName());
			if (bVP && B.Widget->GetCachedWidget().IsValid()) { J->SetArrayField(TEXT("rect"), Rect(B.Widget, VP)); }
			Btns.Add(MakeShared<FJsonValueObject>(J));
		}
		Data->SetArrayField(TEXT("buttons"), Btns);
		Data->SetBoolField(TEXT("includeHidden"), bIncludeHidden);
		if (!bIncludeHidden) { Data->SetNumberField(TEXT("hiddenButtons"), HiddenButtons); Data->SetNumberField(TEXT("hiddenWidgets"), HiddenWidgets); }
		return Data;
	}
}

void RegisterUIRuntimeTools(FEWRegistry& R)
{
	R.Register(TEXT("ui_state"),
		TEXT("What a Play In Editor player's UI shows right now: widgets in the viewport, CommonUI activatable widgets and activatable stacks with their widget list and top, and the buttons (UMG Button or CommonButtonBase) with label, page, layer (the stack holding the page), enabled and rectangle in viewport pixels. By default it lists only what the player can use: visible, on the top page of its stack, page activated. A closed or covered page (for example the old Back/Save after Settings closed) is left out and counted in hiddenButtons / hiddenWidgets. includeHidden lists everything, with top, visible, layer and hiddenReason per item."),
		TEXT(R"({"type":"object","properties":{
			"player":{"type":"integer","description":"Local player index, default 0."},
			"includeHidden":{"type":"boolean","description":"Also list widgets and buttons that are hidden, covered by a higher page, or on a deactivated page (each with layer, top, visible and hiddenReason). Default false."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Err;
			APlayerController* PC = EW::PiePlayer((int32)EW::Num(Args, TEXT("player"), 0), Err);
			if (!PC) { return Bad(TEXT("NO_PLAYER"), Err); }
			return FEWResult::Ok(UiState(PC, EW::Bool(Args, TEXT("includeHidden"), false)));
		});

	R.RegisterAsync(TEXT("ui_click"),
		TEXT("Click a button the player can use in Play In Editor (visible, on the top page of its stack), found by widget name or label (wildcards allowed). method click (default) first checks that no other hit-testable widget is drawn over the button center (else COVERED), then presses the Slate button (press + click, as a pointer or gamepad accept does); method event fires the UMG OnClicked directly, even when covered. Returns the UI state after settleSeconds."),
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

			// Only buttons the player can use now (visible, top page of its stack) are candidates; a button on a
			// closed or covered page is reported as such instead of being clicked.
			const TArray<FButtonInfo> All = Buttons(PC, /*bIncludeOffscreen*/ true);
			auto Matches = [&Key](const FButtonInfo& B) { return B.Widget->GetName() == Key || B.Label.Equals(Key, ESearchCase::IgnoreCase) || B.Label.MatchesWildcard(Key) || B.Widget->GetName().MatchesWildcard(Key); };
			auto Describe = [](const FButtonInfo& B) { return FString::Printf(TEXT("%s (page %s, layer %s)"), *B.Widget->GetName(), *B.Owner, *B.Layer); };
			TArray<FButtonInfo> Hits, Unusable;
			TArray<FString> Names;
			for (const FButtonInfo& B : All)
			{
				if (!IsUsable(B)) { if (Matches(B)) { Unusable.Add(B); } continue; }
				Names.Add(B.Label.IsEmpty() ? B.Widget->GetName() : B.Label);
				if (Matches(B)) { Hits.Add(B); }
			}
			if (Hits.Num() == 0)
			{
				if (Unusable.Num() > 0)
				{
					TArray<FString> Where;
					for (const FButtonInfo& B : Unusable) { Where.Add(FString::Printf(TEXT("%s: %s"), *Describe(B), *B.HiddenReason)); }
					Done(Bad(TEXT("BUTTON_NOT_FOUND"), FString::Printf(TEXT("'%s' exists only on a page the player cannot use now (see ui_state includeHidden)."), *Key), Where));
					return;
				}
				Done(Bad(TEXT("BUTTON_NOT_FOUND"), FString::Printf(TEXT("No visible button '%s' on player %d's screen."), *Key, (int32)EW::Num(Args, TEXT("player"), 0)), EW::Suggest(Key, Names)));
				return;
			}
			if (Hits.Num() > 1)
			{
				TArray<FString> Which;
				for (const FButtonInfo& B : Hits) { Which.Add(Describe(B)); }
				Done(Bad(TEXT("AMBIGUOUS"), FString::Printf(TEXT("%d buttons match '%s'; pass the widget name. Candidates are listed as name (page, layer)."), Hits.Num(), *Key), Which));
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

	R.RegisterAsync(TEXT("ui_key"),
		TEXT("Send a key or a mouse button to a Play In Editor player's UI through Slate, the same path a keyboard or mouse takes: the key goes to the widget that has user focus (UMG NativeOnKeyDown / OnKeyDown, CommonUI input, text boxes) or, with no focused widget, to the game viewport. Use it for panels that wait for 'press any key' or for a focused widget that ui_click cannot reach. key (e.g. Enter, SpaceBar, E, Escape, Gamepad_FaceButton_Bottom) with event tap (default: press, hold, release), press or release; or mouseButton (Left, Right, Middle) at x, y in viewport pixels. focus first moves user focus to a widget by name or class (wildcards allowed) and fails with FOCUS_FAILED when the widget cannot take focus. Returns what had focus before and after, whether Slate handled the event, and the UI state after settleSeconds. Use pie_input instead for gameplay input (Enhanced Input actions)."),
		TEXT(R"({"type":"object","properties":{
			"player":{"type":"integer","description":"Play In Editor player index (0 = first, default 0)."},
			"key":{"type":"string","description":"Key name, e.g. Enter, SpaceBar, E, Escape, Gamepad_FaceButton_Bottom."},
			"event":{"type":"string","enum":["tap","press","release"],"description":"tap (default): press, hold for seconds, release."},
			"seconds":{"type":"number","description":"Hold time for tap, default 0.05. Max 5."},
			"mouseButton":{"type":"string","enum":["Left","Right","Middle"],"description":"Send a mouse button instead of a key. Needs x and y."},
			"x":{"type":"number","description":"Mouse x in viewport pixels (as in ui_state rect)."},
			"y":{"type":"number","description":"Mouse y in viewport pixels."},
			"focus":{"type":"string","description":"Optional: widget name or class (wildcards allowed) to give user focus before the input."},
			"settleSeconds":{"type":"number","description":"Wait after the input before reading the UI, default 0.3."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args, FEWDone Done)
		{
			FString Err;
			APlayerController* PC = EW::PiePlayer((int32)EW::Num(Args, TEXT("player"), 0), Err);
			if (!PC) { Done(Bad(TEXT("NO_PLAYER"), Err)); return; }
			if (!FSlateApplication::IsInitialized()) { Done(Bad(TEXT("NO_SLATE"), TEXT("Slate is not available."))); return; }
			const FString KeyName = EW::Str(Args, TEXT("key"));
			const FString MouseName = EW::Str(Args, TEXT("mouseButton"));
			if (KeyName.IsEmpty() == MouseName.IsEmpty()) { Done(Bad(TEXT("INVALID_ARGUMENT"), TEXT("Pass exactly one of key or mouseButton."))); return; }
			const FString Event = Args->HasField(TEXT("event")) ? EW::Str(Args, TEXT("event")).ToLower() : TEXT("tap");
			if (Event != TEXT("tap") && Event != TEXT("press") && Event != TEXT("release")) { Done(Bad(TEXT("INVALID_ARGUMENT"), TEXT("event must be tap, press or release."))); return; }
			const float Hold = FMath::Clamp((float)EW::Num(Args, TEXT("seconds"), 0.05), 0.f, 5.f);
			const float Settle = FMath::Clamp((float)EW::Num(Args, TEXT("settleSeconds"), 0.3), 0.f, 10.f);
			const int32 UserIndex = (PC->GetLocalPlayer() && PC->GetLocalPlayer()->GetSlateUser().IsValid()) ? PC->GetLocalPlayer()->GetSlateUser()->GetUserIndex() : 0;

			FKey Key;
			if (!KeyName.IsEmpty() && !ParseKeyName(KeyName, Key)) { Done(Bad(TEXT("UNKNOWN_KEY"), FString::Printf(TEXT("No key '%s'."), *KeyName), SuggestKeys(KeyName))); return; }
			FKey Mouse;
			FVector2D MouseAbs = FVector2D::ZeroVector;
			TSharedPtr<SWindow> MouseWindow;
			if (!MouseName.IsEmpty())
			{
				Mouse = MouseName == TEXT("Left") ? EKeys::LeftMouseButton : MouseName == TEXT("Right") ? EKeys::RightMouseButton : EKeys::MiddleMouseButton;
				if (!Args->HasField(TEXT("x")) || !Args->HasField(TEXT("y"))) { Done(Bad(TEXT("INVALID_ARGUMENT"), TEXT("mouseButton needs x and y (viewport pixels)."))); return; }
				FGeometry VP;
				if (!ViewportGeometry(PC, VP)) { Done(Bad(TEXT("NO_VIEWPORT"), TEXT("The player has no game viewport."))); return; }
				MouseAbs = VP.LocalToAbsolute(FVector2D(EW::Num(Args, TEXT("x"), 0), EW::Num(Args, TEXT("y"), 0)));
				UGameViewportClient* GVC = PC->GetWorld()->GetGameViewport();
				MouseWindow = GVC && GVC->GetGameViewportWidget().IsValid() ? FSlateApplication::Get().FindWidgetWindow(GVC->GetGameViewportWidget().ToSharedRef()) : nullptr;
				if (!MouseWindow.IsValid() || !MouseWindow->GetNativeWindow().IsValid()) { Done(Bad(TEXT("NO_VIEWPORT"), TEXT("The game viewport has no window."))); return; }
			}

			// Optional focus move.
			const FString FocusKey = EW::Str(Args, TEXT("focus"));
			if (!FocusKey.IsEmpty())
			{
				TArray<FString> Names;
				UWidget* Target = nullptr;
				for (TObjectIterator<UWidget> It; It; ++It)
				{
					UWidget* W = *It;
					if (!IsValid(W) || W->GetWorld() != PC->GetWorld() || !W->GetCachedWidget().IsValid()) { continue; }
					const FString Cls = W->GetClass()->GetName();
					Names.Add(W->GetName());
					if (!Target && (W->GetName() == FocusKey || Cls == FocusKey || Cls == FocusKey + TEXT("_C") || W->GetName().MatchesWildcard(FocusKey) || Cls.MatchesWildcard(FocusKey))) { Target = W; }
				}
				if (!Target) { Done(Bad(TEXT("WIDGET_NOT_FOUND"), FString::Printf(TEXT("No widget '%s' on player's screen."), *FocusKey), EW::Suggest(FocusKey, Names))); return; }
				Target->SetUserFocus(PC);
				TSharedPtr<SWidget> Now = FSlateApplication::Get().GetUserFocusedWidget(UserIndex);
				bool bInside = false;
				for (TSharedPtr<SWidget> P = Now; P.IsValid(); P = P->GetParentWidget()) { if (P == Target->GetCachedWidget()) { bInside = true; break; } }
				if (!bInside) { Done(Bad(TEXT("FOCUS_FAILED"), FString::Printf(TEXT("%s did not take user focus. A UserWidget needs bIsFocusable (or a focusable child such as a button); the focus is on %s."), *Target->GetName(), *FocusInfo(PC, UserIndex)->GetStringField(TEXT("slateType"))))); return; }
			}

			TSharedRef<FJsonObject> Sent = MakeShared<FJsonObject>();
			Sent->SetObjectField(TEXT("focusBefore"), FocusInfo(PC, UserIndex));
			Sent->SetStringField(TEXT("event"), Event);
			TWeakObjectPtr<APlayerController> WeakPC = PC;

			auto Finish = [Done, Sent, WeakPC, Settle, UserIndex]()
			{
				EW::PollUntil(Settle, []() { return false; }, [Done, Sent, WeakPC, UserIndex]()
				{
					if (!WeakPC.IsValid()) { Done(Bad(TEXT("NO_PLAYER"), TEXT("The player went away after the input."))); return; }
					Sent->SetObjectField(TEXT("focusAfter"), FocusInfo(WeakPC.Get(), UserIndex));
					TSharedRef<FJsonObject> Data = UiState(WeakPC.Get());
					Data->SetObjectField(TEXT("sent"), Sent);
					Done(FEWResult::Ok(Data));
				});
			};

			if (!KeyName.IsEmpty())
			{
				Sent->SetStringField(TEXT("key"), Key.GetFName().ToString());
				const uint32* KeyCode = nullptr; const uint32* CharCode = nullptr;
				FInputKeyManager::Get().GetCodesFromKey(Key, KeyCode, CharCode);
				const uint32 KC = KeyCode ? *KeyCode : 0, CC = CharCode ? *CharCode : 0;
				const FKeyEvent Down(Key, FModifierKeysState(), (uint32)UserIndex, false, CC, KC);
				const FKeyEvent Up(Key, FModifierKeysState(), (uint32)UserIndex, false, CC, KC);
				if (Event != TEXT("release"))
				{
					Sent->SetBoolField(TEXT("handled"), FSlateApplication::Get().ProcessKeyDownEvent(Down));
					if (CC >= 32 && CC != 127) { FSlateApplication::Get().ProcessKeyCharEvent(FCharacterEvent((TCHAR)CC, FModifierKeysState(), (uint32)UserIndex, false)); }
				}
				if (Event == TEXT("press")) { Finish(); return; }
				if (Event == TEXT("release")) { Sent->SetBoolField(TEXT("handled"), FSlateApplication::Get().ProcessKeyUpEvent(Up)); Finish(); return; }
				EW::PollUntil(Hold, []() { return false; }, [Up, Finish]()
				{
					FSlateApplication::Get().ProcessKeyUpEvent(Up);
					Finish();
				});
				return;
			}

			Sent->SetStringField(TEXT("mouseButton"), MouseName);
			Sent->SetNumberField(TEXT("x"), EW::Num(Args, TEXT("x"), 0));
			Sent->SetNumberField(TEXT("y"), EW::Num(Args, TEXT("y"), 0));
			const TSharedPtr<FGenericWindow> Native = MouseWindow->GetNativeWindow();
			const FVector2D AbsPos = MouseAbs;
			FSlateApplication& Slate = FSlateApplication::Get();
			Slate.ProcessMouseMoveEvent(FPointerEvent(0, AbsPos, AbsPos, TSet<FKey>(), FKey(), 0.f, FModifierKeysState()), /*bIsSynthetic*/ true);
			const TSet<FKey> Pressed({ Mouse });
			const FPointerEvent DownEv(0, AbsPos, AbsPos, Pressed, Mouse, 0.f, FModifierKeysState());
			const FPointerEvent UpEv(0, AbsPos, AbsPos, TSet<FKey>(), Mouse, 0.f, FModifierKeysState());
			if (Event != TEXT("release")) { Sent->SetBoolField(TEXT("handled"), Slate.ProcessMouseButtonDownEvent(Native, DownEv)); }
			if (Event == TEXT("press")) { Finish(); return; }
			if (Event == TEXT("release")) { Sent->SetBoolField(TEXT("handled"), Slate.ProcessMouseButtonUpEvent(UpEv)); Finish(); return; }
			EW::PollUntil(Hold, []() { return false; }, [UpEv, Finish]()
			{
				FSlateApplication::Get().ProcessMouseButtonUpEvent(UpEv);
				Finish();
			});
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
