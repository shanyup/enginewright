// UMG: create Widget Blueprints, read and edit their widget tree (widgets, slots, properties), render one to PNG.
#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EWProps.h"

#include "AssetToolsModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "Editor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "IAssetTools.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Base64.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "Slate/WidgetRenderer.h"
#include "TextureResource.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"

namespace
{
	FEWResult Bad(const FString& Code, const FString& Msg, const TArray<FString>& Sug = {}) { return FEWResult::Error(Code, Msg, Sug); }

	UWidgetBlueprint* LoadWB(const TSharedPtr<FJsonObject>& Args, FEWResult& OutErr)
	{
		FString Err;
		UWidgetBlueprint* WB = EW::LoadAs<UWidgetBlueprint>(EW::Str(Args, TEXT("path")), Err);
		if (!WB) { OutErr = Bad(TEXT("ASSET_NOT_FOUND"), Err); return nullptr; }
		if (!WB->WidgetTree) { OutErr = Bad(TEXT("INVALID_ASSET"), TEXT("The Widget Blueprint has no widget tree.")); return nullptr; }
		return WB;
	}

	TArray<FString> WidgetNames(UWidgetBlueprint* WB)
	{
		TArray<UWidget*> All;
		WB->WidgetTree->GetAllWidgets(All);
		TArray<FString> Out;
		for (UWidget* W : All) { if (W) { Out.Add(W->GetName()); } }
		return Out;
	}

	UWidget* FindW(UWidgetBlueprint* WB, const FString& Name, FEWResult& OutErr)
	{
		UWidget* W = WB->WidgetTree->FindWidget(FName(*Name));
		if (!W) { OutErr = Bad(TEXT("WIDGET_NOT_FOUND"), FString::Printf(TEXT("No widget '%s' in %s."), *Name, *WB->GetName()), EW::Suggest(Name, WidgetNames(WB))); }
		return W;
	}

	TArray<TSharedPtr<FJsonValue>> Vec2(const FVector2D& V) { return { MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y) }; }

	bool ReadVec2(const TSharedPtr<FJsonValue>& V, FVector2D& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (!V.IsValid() || !V->TryGetArray(A) || A->Num() != 2) { return false; }
		double X, Y;
		if (!(*A)[0]->TryGetNumber(X) || !(*A)[1]->TryGetNumber(Y)) { return false; }
		Out = FVector2D(X, Y);
		return true;
	}

	TSharedRef<FJsonObject> SlotJson(UPanelSlot* Slot)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		if (!Slot) { return J; }
		J->SetStringField(TEXT("class"), Slot->GetClass()->GetName());
		if (UCanvasPanelSlot* C = Cast<UCanvasPanelSlot>(Slot))
		{
			J->SetArrayField(TEXT("position"), Vec2(C->GetPosition()));
			J->SetArrayField(TEXT("size"), Vec2(C->GetSize()));
			const FAnchors A = C->GetAnchors();
			J->SetArrayField(TEXT("anchors"), { MakeShared<FJsonValueNumber>(A.Minimum.X), MakeShared<FJsonValueNumber>(A.Minimum.Y), MakeShared<FJsonValueNumber>(A.Maximum.X), MakeShared<FJsonValueNumber>(A.Maximum.Y) });
			J->SetArrayField(TEXT("alignment"), Vec2(C->GetAlignment()));
			J->SetBoolField(TEXT("autoSize"), C->GetAutoSize());
			J->SetNumberField(TEXT("zOrder"), C->GetZOrder());
			return J;
		}
		TSharedPtr<FJsonObject> Values;
		if (EW::ReadValues(Slot->GetClass(), Slot, {}, Values).bOk && Values.IsValid()) { J->SetObjectField(TEXT("values"), Values); }
		return J;
	}

	TSharedRef<FJsonObject> WidgetJson(UWidget* W)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("name"), W->GetName());
		J->SetStringField(TEXT("class"), W->GetClass()->GetName());
		int32 Index = INDEX_NONE;
		if (UPanelWidget* P = UWidgetTree::FindWidgetParent(W, Index)) { J->SetStringField(TEXT("parent"), P->GetName()); J->SetNumberField(TEXT("index"), Index); }
		J->SetBoolField(TEXT("isVariable"), W->bIsVariable);
		J->SetStringField(TEXT("visibility"), StaticEnum<ESlateVisibility>()->GetNameStringByValue((int64)W->GetVisibility()));
		if (UTextBlock* T = Cast<UTextBlock>(W)) { J->SetStringField(TEXT("text"), T->GetText().ToString()); }
		if (W->Slot) { J->SetObjectField(TEXT("slot"), SlotJson(W->Slot)); }
		return J;
	}

	/** Canvas slot keys handled here; every other key goes through the strict property writer. */
	FEWResult WriteSlot(UPanelSlot* Slot, const TSharedPtr<FJsonObject>& In)
	{
		if (!Slot) { return Bad(TEXT("NO_SLOT"), TEXT("This widget is the root: it has no slot.")); }
		TSharedPtr<FJsonObject> Rest = MakeShared<FJsonObject>(*In);
		Slot->Modify();
		if (UCanvasPanelSlot* C = Cast<UCanvasPanelSlot>(Slot))
		{
			FVector2D V;
			if (TSharedPtr<FJsonValue> P = In->TryGetField(TEXT("position"))) { if (!ReadVec2(P, V)) { return Bad(TEXT("INVALID_VALUE"), TEXT("slot.position must be [x, y].")); } C->SetPosition(V); Rest->RemoveField(TEXT("position")); }
			if (TSharedPtr<FJsonValue> P = In->TryGetField(TEXT("size"))) { if (!ReadVec2(P, V)) { return Bad(TEXT("INVALID_VALUE"), TEXT("slot.size must be [width, height].")); } C->SetSize(V); Rest->RemoveField(TEXT("size")); }
			if (TSharedPtr<FJsonValue> P = In->TryGetField(TEXT("alignment"))) { if (!ReadVec2(P, V)) { return Bad(TEXT("INVALID_VALUE"), TEXT("slot.alignment must be [x, y] (0..1).")); } C->SetAlignment(V); Rest->RemoveField(TEXT("alignment")); }
			if (TSharedPtr<FJsonValue> P = In->TryGetField(TEXT("anchors")))
			{
				const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
				if (!P->TryGetArray(A) || A->Num() != 4) { return Bad(TEXT("INVALID_VALUE"), TEXT("slot.anchors must be [minX, minY, maxX, maxY] (0..1).")); }
				C->SetAnchors(FAnchors((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber(), (*A)[3]->AsNumber()));
				Rest->RemoveField(TEXT("anchors"));
			}
			bool B;
			if (In->TryGetBoolField(TEXT("autoSize"), B)) { C->SetAutoSize(B); Rest->RemoveField(TEXT("autoSize")); }
			double Z;
			if (In->TryGetNumberField(TEXT("zOrder"), Z)) { C->SetZOrder((int32)Z); Rest->RemoveField(TEXT("zOrder")); }
		}
		if (Rest->Values.Num())
		{
			FEWResult R = EW::WriteValues(Slot->GetClass(), Slot, Slot, Rest);
			if (!R.bOk) { return R; }
		}
		Slot->SynchronizeProperties();
		return FEWResult::Ok();
	}

	FEWResult FinishWB(UWidgetBlueprint* WB, FEWResult Res)
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
		if (Errors.Num() || WB->Status == BS_Error)
		{
			Res.bOk = false;
			Res.ErrorCode = TEXT("COMPILE_ERROR");
			Res.Message = TEXT("The change was applied but the Widget Blueprint does not compile; see data.compileErrors.");
			Res.Data->SetArrayField(TEXT("compileErrors"), Errors);
		}
		bool bDeferred = false;
		const bool bSaved = EW::SaveOrDefer(WB->GetOutermost(), bDeferred);
		Res.Data->SetBoolField(TEXT("saved"), bSaved && !bDeferred);
		if (!bSaved) { Res.Warnings.Add(TEXT("Saving the Widget Blueprint failed.")); }
		return Res;
	}

	UClass* WidgetClass(const FString& Name, FEWResult& OutErr)
	{
		FString Err;
		TArray<FString> Sug;
		UClass* C = EW::FindClass(Name, Err, &Sug);
		if (!C) { OutErr = Bad(TEXT("UNKNOWN_CLASS"), Err, Sug); return nullptr; }
		if (!C->IsChildOf(UWidget::StaticClass())) { OutErr = Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("%s is not a widget class (TextBlock, Button, Image, VerticalBox, CanvasPanel, a Widget Blueprint...)."), *C->GetName())); return nullptr; }
		if (C->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) { OutErr = Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("%s is abstract or outdated."), *C->GetName())); return nullptr; }
		return C;
	}
}

void RegisterUMGTools(FEWRegistry& R)
{
	R.Register(TEXT("widget_create"),
		TEXT("Create a Widget Blueprint (UMG) with a root panel (default CanvasPanel), compile and save it."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string","description":"New asset path, e.g. /Game/UI/WBP_Menu."},
			"root":{"type":"string","description":"Root panel class: CanvasPanel (default), VerticalBox, Overlay, ... or none."},
			"parent":{"type":"string","description":"Parent class, default UserWidget."}},
			"required":["path"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Folder, Name, Err;
			if (!EW::SplitAssetPath(EW::Str(Args, TEXT("path")), Folder, Name, Err)) { return Bad(TEXT("INVALID_PATH"), Err); }
			if (EW::AssetPathTaken(Folder / Name)) { return Bad(TEXT("ASSET_EXISTS"), FString::Printf(TEXT("'%s' already exists."), *(Folder / Name))); }
			UClass* Parent = UUserWidget::StaticClass();
			if (Args->HasField(TEXT("parent")))
			{
				TArray<FString> Sug;
				Parent = EW::FindClass(EW::Str(Args, TEXT("parent")), Err, &Sug);
				if (!Parent) { return Bad(TEXT("UNKNOWN_CLASS"), Err, Sug); }
				if (!Parent->IsChildOf(UUserWidget::StaticClass())) { return Bad(TEXT("INVALID_PARENT"), FString::Printf(TEXT("%s is not a UserWidget class."), *Parent->GetName())); }
			}
			FString RootName = Args->HasField(TEXT("root")) ? EW::Str(Args, TEXT("root")) : TEXT("CanvasPanel");
			UClass* Root = nullptr;
			if (!RootName.Equals(TEXT("none"), ESearchCase::IgnoreCase))
			{
				FEWResult CErr;
				Root = WidgetClass(RootName, CErr);
				if (!Root) { return CErr; }
				if (!Root->IsChildOf(UPanelWidget::StaticClass())) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("root must be a panel (CanvasPanel, VerticalBox, Overlay...); %s is not."), *Root->GetName())); }
			}
			UWidgetBlueprintFactory* F = NewObject<UWidgetBlueprintFactory>();
			F->ParentClass = Parent;
			UWidgetBlueprint* WB = Cast<UWidgetBlueprint>(FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().CreateAsset(Name, Folder, UWidgetBlueprint::StaticClass(), F));
			if (!WB || !WB->WidgetTree) { return Bad(TEXT("CREATE_FAILED"), TEXT("The editor could not create the Widget Blueprint.")); }
			// The factory uses the project's default root; replace it with the one asked for.
			UWidget* OldRoot = WB->WidgetTree->RootWidget;
			if (OldRoot && OldRoot->GetClass() != Root)
			{
				WB->WidgetTree->RemoveWidget(OldRoot);
				WB->OnVariableRemoved(OldRoot->GetFName());
				OldRoot->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
				WB->WidgetTree->RootWidget = nullptr;
			}
			if (Root && !WB->WidgetTree->RootWidget)
			{
				WB->WidgetTree->RootWidget = WB->WidgetTree->ConstructWidget<UWidget>(Root);
				WB->OnVariableAdded(WB->WidgetTree->RootWidget->GetFName());
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("path"), WB->GetPathName());
			Data->SetStringField(TEXT("root"), WB->WidgetTree && WB->WidgetTree->RootWidget ? WB->WidgetTree->RootWidget->GetName() : FString());
			return FinishWB(WB, FEWResult::Ok(Data));
		}, /*bTransactional*/ false);

	R.Register(TEXT("widget_tree"),
		TEXT("Read a Widget Blueprint's widget tree: every widget with class, parent, index, variable flag, visibility, text (TextBlock) and slot layout (canvas position, size, anchors, alignment; padding and alignment for box slots)."),
		TEXT(R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UWidgetBlueprint* WB = LoadWB(Args, Err);
			if (!WB) { return Err; }
			TArray<UWidget*> All;
			WB->WidgetTree->GetAllWidgets(All);
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (UWidget* W : All) { if (W) { Rows.Add(MakeShared<FJsonValueObject>(WidgetJson(W))); } }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("path"), WB->GetPathName());
			Data->SetStringField(TEXT("root"), WB->WidgetTree->RootWidget ? WB->WidgetTree->RootWidget->GetName() : FString());
			Data->SetArrayField(TEXT("widgets"), Rows);
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("widget_get"),
		TEXT("Read property values of one widget (all editable ones, or the names given) plus its slot."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string"},"name":{"type":"string","description":"Widget name."},
			"properties":{"type":"array","items":{"type":"string"}}},
			"required":["path","name"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UWidgetBlueprint* WB = LoadWB(Args, Err);
			if (!WB) { return Err; }
			UWidget* W = FindW(WB, EW::Str(Args, TEXT("name")), Err);
			if (!W) { return Err; }
			TSharedPtr<FJsonObject> Values;
			FEWResult RR = EW::ReadValues(W->GetClass(), W, EW::StrList(Args, TEXT("properties")), Values);
			if (!RR.bOk) { return RR; }
			TSharedRef<FJsonObject> Data = WidgetJson(W);
			Data->SetObjectField(TEXT("values"), Values.IsValid() ? Values : MakeShared<FJsonObject>());
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("widget_add"),
		TEXT("Add a widget (TextBlock, Button, Image, VerticalBox, HorizontalBox, Overlay, SizeBox, Border, Spacer, a Widget Blueprint...) under a panel, with property values and slot layout checked like object_set; compile, save, and return the widget as read back. Without a parent it goes under the root (or becomes the root when there is none)."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string"},
			"class":{"type":"string"},
			"name":{"type":"string","description":"Unique widget name."},
			"parent":{"type":"string","description":"Panel widget name; default the root."},
			"index":{"type":"integer","description":"Insert position among the parent's children; default last."},
			"isVariable":{"type":"boolean","description":"Expose as a Blueprint variable (default true)."},
			"properties":{"type":"object","description":"e.g. {\"Text\": \"Play\", \"Visibility\": \"Visible\"}."},
			"slot":{"type":"object","description":"Canvas: position [x,y], size [w,h], anchors [minX,minY,maxX,maxY], alignment [x,y], autoSize, zOrder. Box/overlay slots: Padding, HorizontalAlignment, VerticalAlignment, Size."}},
			"required":["path","class","name"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UWidgetBlueprint* WB = LoadWB(Args, Err);
			if (!WB) { return Err; }
			UClass* C = WidgetClass(EW::Str(Args, TEXT("class")), Err);
			if (!C) { return Err; }
			if (C == WB->GeneratedClass) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("A Widget Blueprint cannot contain itself.")); }
			const FString Name = EW::Str(Args, TEXT("name"));
			if (Name.IsEmpty() || !FName(*Name).IsValidXName(INVALID_OBJECTNAME_CHARACTERS)) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("'%s' is not a valid widget name."), *Name)); }
			if (WB->WidgetTree->FindWidget(FName(*Name))) { return Bad(TEXT("WIDGET_EXISTS"), FString::Printf(TEXT("A widget named '%s' already exists."), *Name)); }

			UPanelWidget* Parent = nullptr;
			const FString ParentName = EW::Str(Args, TEXT("parent"));
			if (!ParentName.IsEmpty())
			{
				UWidget* PW = FindW(WB, ParentName, Err);
				if (!PW) { return Err; }
				Parent = Cast<UPanelWidget>(PW);
				if (!Parent) { return Bad(TEXT("NOT_A_PANEL"), FString::Printf(TEXT("'%s' (%s) cannot hold children."), *ParentName, *PW->GetClass()->GetName())); }
			}
			else if (WB->WidgetTree->RootWidget)
			{
				Parent = Cast<UPanelWidget>(WB->WidgetTree->RootWidget);
				if (!Parent) { return Bad(TEXT("NOT_A_PANEL"), TEXT("The root widget is not a panel; pass parent.")); }
			}
			if (Parent && !Parent->CanAddMoreChildren()) { return Bad(TEXT("PANEL_FULL"), FString::Printf(TEXT("'%s' (%s) takes only one child and already has it."), *Parent->GetName(), *Parent->GetClass()->GetName())); }

			const TSharedPtr<FJsonObject>* Props = nullptr;
			Args->TryGetObjectField(TEXT("properties"), Props);
			const TSharedPtr<FJsonObject>* SlotArgs = nullptr;
			Args->TryGetObjectField(TEXT("slot"), SlotArgs);
			if (SlotArgs && !Parent) { return Bad(TEXT("NO_SLOT"), TEXT("The new widget becomes the root: it has no slot.")); }

			WB->Modify();
			WB->WidgetTree->Modify();
			UWidget* W = WB->WidgetTree->ConstructWidget<UWidget>(C, FName(*Name));
			if (!W) { return Bad(TEXT("CREATE_FAILED"), TEXT("The widget could not be constructed.")); }
			W->bIsVariable = EW::Bool(Args, TEXT("isVariable"), true);
			WB->OnVariableAdded(W->GetFName());
			auto Undo = [&]() { WB->WidgetTree->RemoveWidget(W); WB->OnVariableRemoved(W->GetFName()); W->Rename(nullptr, GetTransientPackage()); };
			if (Props && (*Props)->Values.Num())
			{
				FEWResult PR = EW::WriteValues(C, W, W, *Props);
				if (!PR.bOk) { Undo(); return PR; }
			}
			if (Parent)
			{
				const int32 Index = (int32)EW::Num(Args, TEXT("index"), -1);
				Parent->Modify();
				UPanelSlot* Slot = (Index >= 0 && Index < Parent->GetChildrenCount()) ? Parent->InsertChildAt(Index, W) : Parent->AddChild(W);
				if (!Slot) { Undo(); return Bad(TEXT("ADD_FAILED"), TEXT("The parent refused the widget.")); }
				if (SlotArgs)
				{
					FEWResult SR = WriteSlot(Slot, *SlotArgs);
					if (!SR.bOk) { Parent->RemoveChild(W); Undo(); return SR; }
				}
			}
			else
			{
				WB->WidgetTree->RootWidget = W;
			}
			W->SynchronizeProperties();
			return FinishWB(WB, FEWResult::Ok(WidgetJson(W)));
		}, /*bTransactional*/ false);

	R.Register(TEXT("widget_set"),
		TEXT("Change one widget's properties and/or slot layout (same rules as widget_add), then compile, save and read back. Nothing changes when any value is invalid."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string"},"name":{"type":"string"},
			"properties":{"type":"object"},"slot":{"type":"object"},
			"isVariable":{"type":"boolean"}},
			"required":["path","name"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UWidgetBlueprint* WB = LoadWB(Args, Err);
			if (!WB) { return Err; }
			UWidget* W = FindW(WB, EW::Str(Args, TEXT("name")), Err);
			if (!W) { return Err; }
			const TSharedPtr<FJsonObject>* Props = nullptr;
			const TSharedPtr<FJsonObject>* SlotArgs = nullptr;
			Args->TryGetObjectField(TEXT("properties"), Props);
			Args->TryGetObjectField(TEXT("slot"), SlotArgs);
			if (!Props && !SlotArgs && !Args->HasField(TEXT("isVariable"))) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("Pass properties, slot or isVariable.")); }
			if (SlotArgs && !W->Slot) { return Bad(TEXT("NO_SLOT"), TEXT("This widget is the root: it has no slot.")); }
			// Keep a copy to restore when the slot write fails after the properties were written.
			UWidget* Backup = DuplicateObject<UWidget>(W, GetTransientPackage());
			UPanelSlot* SlotBackup = W->Slot ? DuplicateObject<UPanelSlot>(W->Slot, GetTransientPackage()) : nullptr;
			W->Modify();
			FEWResult Res = FEWResult::Ok();
			TSharedRef<FJsonObject> Changes = MakeShared<FJsonObject>();
			if (Props && (*Props)->Values.Num())
			{
				FEWResult PR = EW::WriteValues(W->GetClass(), W, W, *Props);
				if (!PR.bOk) { return PR; }
				if (PR.Data.IsValid()) { Changes->SetObjectField(TEXT("properties"), PR.Data); }
			}
			if (SlotArgs)
			{
				FEWResult SR = WriteSlot(W->Slot, *SlotArgs);
				if (!SR.bOk)
				{
					UEngine::CopyPropertiesForUnrelatedObjects(Backup, W);
					if (SlotBackup) { UEngine::CopyPropertiesForUnrelatedObjects(SlotBackup, W->Slot); W->Slot->SynchronizeProperties(); }
					W->SynchronizeProperties();
					return SR;
				}
			}
			if (Args->HasField(TEXT("isVariable"))) { W->bIsVariable = EW::Bool(Args, TEXT("isVariable"), W->bIsVariable); }
			W->SynchronizeProperties();
			TSharedRef<FJsonObject> Data = WidgetJson(W);
			Data->SetObjectField(TEXT("changes"), Changes);
			Res.Data = Data;
			return FinishWB(WB, Res);
		}, /*bTransactional*/ false);

	R.Register(TEXT("widget_remove"),
		TEXT("Remove a widget and everything under it from a Widget Blueprint; compile and save. Lists the removed widgets."),
		TEXT(R"({"type":"object","properties":{"path":{"type":"string"},"name":{"type":"string"}},"required":["path","name"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UWidgetBlueprint* WB = LoadWB(Args, Err);
			if (!WB) { return Err; }
			UWidget* W = FindW(WB, EW::Str(Args, TEXT("name")), Err);
			if (!W) { return Err; }
			TArray<UWidget*> Gone;
			Gone.Add(W);
			if (UPanelWidget* P = Cast<UPanelWidget>(W)) { UWidgetTree::GetChildWidgets(P, Gone); }
			TArray<TSharedPtr<FJsonValue>> Names;
			for (UWidget* G : Gone) { Names.Add(MakeShared<FJsonValueString>(G->GetName())); }
			WB->Modify();
			WB->WidgetTree->Modify();
			if (!WB->WidgetTree->RemoveWidget(W)) { return Bad(TEXT("REMOVE_FAILED"), TEXT("The widget could not be removed.")); }
			for (UWidget* G : Gone) { WB->OnVariableRemoved(G->GetFName()); G->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional); }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("removed"), Names);
			Data->SetNumberField(TEXT("widgetsLeft"), WidgetNames(WB).Num());
			return FinishWB(WB, FEWResult::Ok(Data));
		}, /*bTransactional*/ false);

	R.Register(TEXT("widget_render"),
		TEXT("Render a Widget Blueprint to a PNG at a given resolution (no Play In Editor needed): saves it under Saved/Enginewright/Screenshots and returns it as an image. Use it to compare a screen with a mock-up."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string"},
			"width":{"type":"integer","description":"Default 1920."},"height":{"type":"integer","description":"Default 1080."},
			"inline":{"type":"boolean","description":"Return the image in the reply (default true)."},
			"file":{"type":"string","description":"PNG path; default a new file under Saved/Enginewright/Screenshots."}},
			"required":["path"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWResult Err;
			UWidgetBlueprint* WB = LoadWB(Args, Err);
			if (!WB) { return Err; }
			const int32 W = (int32)EW::Num(Args, TEXT("width"), 1920), H = (int32)EW::Num(Args, TEXT("height"), 1080);
			if (W < 16 || H < 16 || W > 4096 || H > 4096) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("width and height must be 16 to 4096.")); }
			if (!WB->GeneratedClass || WB->Status == BS_Error) { return Bad(TEXT("COMPILE_ERROR"), TEXT("The Widget Blueprint does not compile.")); }
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World) { return Bad(TEXT("NO_LEVEL"), TEXT("No editor world.")); }
			UUserWidget* Inst = CreateWidget<UUserWidget>(World, TSubclassOf<UUserWidget>(WB->GeneratedClass));
			if (!Inst) { return Bad(TEXT("CREATE_FAILED"), TEXT("Could not create the widget.")); }
			TSharedRef<SWidget> Slate = Inst->TakeWidget();
			FWidgetRenderer* Renderer = new FWidgetRenderer(/*bUseGammaCorrection*/ true);
			UTextureRenderTarget2D* RT = FWidgetRenderer::CreateTargetFor(FVector2D(W, H), TF_Bilinear, true);
			RT->ClearColor = FLinearColor::Black;
			RT->UpdateResourceImmediate(true);
			Renderer->DrawWidget(RT, Slate, FVector2D(W, H), 0.f);
			Renderer->DrawWidget(RT, Slate, FVector2D(W, H), 0.016f); // second pass: layout settled
			FlushRenderingCommands();
			TArray<FColor> Pixels;
			FTextureRenderTargetResource* Res = RT->GameThread_GetRenderTargetResource();
			const bool bRead = Res && Res->ReadPixels(Pixels);
			BeginCleanup(Renderer);
			Inst->MarkAsGarbage();
			if (!bRead || Pixels.Num() != W * H) { return Bad(TEXT("CAPTURE_FAILED"), TEXT("Could not read the rendered pixels.")); }
			for (FColor& C : Pixels) { C.A = 255; }
			TArray64<uint8> Png;
			if (!EW::EncodePng(Pixels, W, H, Png)) { return Bad(TEXT("CAPTURE_FAILED"), TEXT("PNG encoding failed.")); }
			FString File = EW::Str(Args, TEXT("file"));
			if (File.IsEmpty()) { File = FPaths::ProjectSavedDir() / TEXT("Enginewright/Screenshots") / FString::Printf(TEXT("%s_%s.png"), *WB->GetName(), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S_%s"))); }
			File = FPaths::ConvertRelativePathToFull(File);
			if (!FFileHelper::SaveArrayToFile(Png, *File)) { return Bad(TEXT("SAVE_FAILED"), FString::Printf(TEXT("Could not write %s."), *File)); }
			int64 NonBlack = 0;
			for (const FColor& C : Pixels) { if (C.R > 8 || C.G > 8 || C.B > 8) { ++NonBlack; } }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("file"), File);
			Data->SetNumberField(TEXT("width"), W);
			Data->SetNumberField(TEXT("height"), H);
			Data->SetNumberField(TEXT("drawnPercent"), FMath::RoundToDouble(10000.0 * NonBlack / Pixels.Num()) / 100);
			FEWResult Out = FEWResult::Ok(Data);
			if (NonBlack == 0) { Out.Warnings.Add(TEXT("The image is empty (nothing visible was drawn).")); }
			if (EW::Bool(Args, TEXT("inline"), true)) { Out.Images.Emplace(TEXT("image/png"), FBase64::Encode(Png.GetData(), Png.Num())); }
			return Out;
		});
}
