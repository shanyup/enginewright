// Editor UI: the status bar indicator (bottom right) and the Enginewright panel (Tools > Enginewright).
// Both read the live server state from FEnginewrightModule and write UEnginewrightSettings (saved per user and project).
#include "EnginewrightModule.h"
#include "EnginewrightSettings.h"
#include "EnginewrightTool.h"
#include "EWCommon.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "ISettingsModule.h"
#include "Misc/MessageDialog.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

namespace EW
{
	void ClientConfigTexts(const FString& Url, const FString& Token, TArray<FString>& OutClients, TArray<FString>& OutTexts, TArray<FString>& OutWhere);
}

namespace
{
	const FName EWPanelTabId(TEXT("EnginewrightPanel"));
	const FName MenuOwner(TEXT("Enginewright"));
	FDelegateHandle MenusHandle;

	const FLinearColor Green(0.15f, 0.75f, 0.25f);
	const FLinearColor Red(0.85f, 0.2f, 0.15f);
	const FLinearColor Grey(0.55f, 0.55f, 0.55f);

	FEnginewrightModule* Module()
	{
		return FModuleManager::GetModulePtr<FEnginewrightModule>(TEXT("Enginewright"));
	}

	/** Change settings the same way Project Settings does: save to the ini, then let the module apply them. */
	void ChangeSettings(TFunctionRef<void(UEnginewrightSettings*)> Edit)
	{
		UEnginewrightSettings* S = GetMutableDefault<UEnginewrightSettings>();
		Edit(S);
		S->SaveConfig();
		UEnginewrightSettings::OnChanged().Broadcast();
	}

	FString Ago(const FDateTime& Time)
	{
		const double Sec = (FDateTime::Now() - Time).GetTotalSeconds();
		if (Sec < 60) { return FString::Printf(TEXT("%.0fs ago"), FMath::Max(0.0, Sec)); }
		if (Sec < 3600) { return FString::Printf(TEXT("%.0f min ago"), Sec / 60); }
		return Time.ToString(TEXT("%H:%M"));
	}

	FString LastCallText()
	{
		const TArray<FEWCallRecord>& Calls = FEWRegistry::Get().RecentCalls();
		if (!Calls.Num()) { return TEXT("none yet"); }
		const FEWCallRecord& C = Calls.Last();
		return FString::Printf(TEXT("%s %s (%.0f ms, %s)"), *C.Tool, C.bOk ? TEXT("ok") : *C.ErrorCode, C.Ms, *Ago(C.Time));
	}

	FLinearColor StateColor()
	{
		const FEnginewrightModule* M = Module();
		return M && M->IsListening() ? Green : Red;
	}

	FString ServerLine()
	{
		const FEnginewrightModule* M = Module();
		if (!M) { return TEXT("Not loaded"); }
		if (M->IsListening()) { return FString::Printf(TEXT("Listening on %s"), *M->GetUrl()); }
		if (!M->GetServerError().IsEmpty()) { return M->GetServerError(); }
		return TEXT("Turned off in the settings. Clients get no answer from this editor.");
	}

	TSharedRef<SWidget> Dot(TAttribute<FSlateColor> Color, float Size = 8.f)
	{
		return SNew(SImage)
			.Image(FAppStyle::Get().GetBrush("Icons.FilledCircle"))
			.DesiredSizeOverride(FVector2D(Size, Size))
			.ColorAndOpacity(Color);
	}

	void OpenPanel()
	{
		FGlobalTabmanager::Get()->TryInvokeTab(EWPanelTabId);
	}

	// ------------------------------------------------------------------------------------------------------------------
	/** Status bar: "Enginewright :3100 (2)" with a green or red dot. Click opens the panel. */
	class SEWStatusIndicator : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SEWStatusIndicator) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&)
		{
			ChildSlot
			[
				SNew(SButton)
				.ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("SimpleButton"))
				.OnClicked_Lambda([]() { OpenPanel(); return FReply::Handled(); })
				.ToolTipText_Lambda([]() { return FText::FromString(TooltipText()); })
				.ContentPadding(FMargin(6.f, 0.f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 5.f, 0.f)
					[
						Dot(TAttribute<FSlateColor>::CreateLambda([]() { return FSlateColor(StateColor()); }))
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock).Text_Lambda([]() { return FText::FromString(LabelText()); })
					]
				]
			];
		}

		static FString LabelText()
		{
			const FEnginewrightModule* M = Module();
			if (!M) { return TEXT("Enginewright"); }
			if (M->IsListening())
			{
				return FString::Printf(TEXT("Enginewright :%u (%d)"), M->GetPort(), M->GetClients().Num());
			}
			if (!M->GetServerError().IsEmpty()) { return FString::Printf(TEXT("Enginewright :%u busy"), M->GetPort()); }
			return TEXT("Enginewright off");
		}

		static FString TooltipText()
		{
			const FEnginewrightModule* M = Module();
			if (!M) { return TEXT("Enginewright is not loaded."); }
			const UEnginewrightSettings* S = GetDefault<UEnginewrightSettings>();
			FString T = ServerLine();
			T += FString::Printf(TEXT("\nToken: %s (Saved/Enginewright/token)"), M->GetToken().IsEmpty() ? TEXT("missing") : TEXT("set"));
			T += FString::Printf(TEXT("\nClients in the last 5 minutes: %d"), M->GetClients().Num());
			T += FString::Printf(TEXT("\nLast call: %s"), *LastCallText());
			if (S->bReadOnly) { T += TEXT("\nRead-only mode: tools that change the project are refused."); }
			T += TEXT("\n\nClick to open the Enginewright panel.");
			return T;
		}
	};

	// ------------------------------------------------------------------------------------------------------------------
	/** The panel: server, token, clients, tool categories, log level, recent calls, doctor. */
	class SEWPanel : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SEWPanel) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&)
		{
			TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);

			// Server
			Body->AddSlot().AutoHeight()[Heading(TEXT("Server"))];
			Body->AddSlot().AutoHeight().Padding(0, 2)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[
					Dot(TAttribute<FSlateColor>::CreateLambda([]() { return FSlateColor(StateColor()); }), 10.f)
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock).AutoWrapText(true).Text_Lambda([]() { return FText::FromString(ServerLine()); })
				]
			];
			Body->AddSlot().AutoHeight().Padding(0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 16, 0)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([]() { return GetDefault<UEnginewrightSettings>()->bServerEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([](ECheckBoxState State)
					{
						ChangeSettings([State](UEnginewrightSettings* S) { S->bServerEnabled = State == ECheckBoxState::Checked; });
					})
					[
						SNew(STextBlock).Text(FText::FromString(TEXT("Server on")))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[
					SNew(STextBlock).Text(FText::FromString(TEXT("Port")))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(90.f)
					[
						SNew(SSpinBox<int32>)
						.MinValue(1024).MaxValue(65535).MinSliderValue(1024).MaxSliderValue(65535).Delta(1)
						.Value_Lambda([]() { return GetDefault<UEnginewrightSettings>()->Port; })
						.OnValueCommitted_Lambda([](int32 Value, ETextCommit::Type)
						{
							if (Value != GetDefault<UEnginewrightSettings>()->Port)
							{
								ChangeSettings([Value](UEnginewrightSettings* S) { S->Port = Value; });
							}
						})
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(8, 0, 0, 0)
				[
					SNew(STextBlock).ColorAndOpacity(Grey)
					.Text_Lambda([]()
					{
						const FEnginewrightModule* M = Module();
						return FText::FromString(M && M->IsPortFromEnvironment()
							? FString::Printf(TEXT("ENGINEWRIGHT_PORT=%u overrides this."), M->GetPort())
							: FString(TEXT("A new port applies at once; copy the client config again.")));
					})
				]
			];

			// Token
			Body->AddSlot().AutoHeight()[Heading(TEXT("Token"))];
			Body->AddSlot().AutoHeight().Padding(0, 2)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
					.Text_Lambda([this]()
					{
						const FEnginewrightModule* M = Module();
						const FString T = M ? M->GetToken() : FString();
						return FText::FromString(bShowToken ? T : FString::Printf(TEXT("%s%s"), *FString::ChrN(24, TEXT('*')), *T.Right(4)));
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
				[
					SNew(SButton).Text_Lambda([this]() { return FText::FromString(bShowToken ? TEXT("Hide") : TEXT("Show")); })
					.OnClicked_Lambda([this]() { bShowToken = !bShowToken; return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
				[
					SNew(SButton).Text(FText::FromString(TEXT("Copy")))
					.OnClicked_Lambda([this]()
					{
						if (const FEnginewrightModule* M = Module()) { FPlatformApplicationMisc::ClipboardCopy(*M->GetToken()); }
						Note(TEXT("Token copied."));
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
				[
					SNew(SButton).Text(FText::FromString(TEXT("New token")))
					.ToolTipText(FText::FromString(TEXT("Make a new random token. Every client must be set up again with the new config.")))
					.OnClicked_Lambda([this]()
					{
						const EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::YesNo,
							FText::FromString(TEXT("Make a new token? Connected MCP clients stop working until you paste the new client config.")));
						if (Answer == EAppReturnType::Yes)
						{
							if (FEnginewrightModule* M = Module()) { M->RegenerateToken(); }
							Note(TEXT("New token made. Copy a client config below and set up your client again."));
						}
						return FReply::Handled();
					})
				]
			];

			// Client setup
			Body->AddSlot().AutoHeight()[Heading(TEXT("Set up a client (copies the config with URL and token)"))];
			TSharedRef<SWrapBox> Copy = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(6, 6));
			static const TCHAR* const Ids[] = { TEXT("claude-code"), TEXT("claude-code-cli"), TEXT("codex"), TEXT("cursor"), TEXT("vscode"), TEXT("hermes") };
			static const TCHAR* const Labels[] = { TEXT("Claude Code (.mcp.json)"), TEXT("Claude Code (command)"), TEXT("Codex"), TEXT("Cursor"), TEXT("VS Code"), TEXT("Hermes") };
			for (int32 i = 0; i < UE_ARRAY_COUNT(Ids); ++i)
			{
				const FString Id = Ids[i];
				const FString Label = Labels[i];
				Copy->AddSlot()
				[
					SNew(SButton).Text(FText::FromString(Label))
					.OnClicked_Lambda([this, Id, Label]()
					{
						const FEnginewrightModule* M = Module();
						if (!M) { return FReply::Handled(); }
						TArray<FString> Clients, Texts, Where;
						EW::ClientConfigTexts(M->GetUrl(), M->GetToken(), Clients, Texts, Where);
						const int32 Index = Clients.IndexOfByKey(Id);
						if (Index != INDEX_NONE)
						{
							FPlatformApplicationMisc::ClipboardCopy(*Texts[Index]);
							Note(FString::Printf(TEXT("Copied the %s config. %s It holds the token: do not commit it."), *Label, *Where[Index]));
						}
						return FReply::Handled();
					})
				];
			}
			Body->AddSlot().AutoHeight().Padding(0, 2)[Copy];

			// Clients
			Body->AddSlot().AutoHeight()[Heading(TEXT("Clients (last 5 minutes)"))];
			Body->AddSlot().AutoHeight()[SAssignNew(ClientList, SVerticalBox)];

			// Tools
			Body->AddSlot().AutoHeight()[Heading(TEXT("Tools"))];
			Body->AddSlot().AutoHeight().Padding(0, 2)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([]() { return GetDefault<UEnginewrightSettings>()->bReadOnly ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([](ECheckBoxState State)
				{
					ChangeSettings([State](UEnginewrightSettings* S) { S->bReadOnly = State == ECheckBoxState::Checked; });
				})
				.ToolTipText(FText::FromString(TEXT("A guard against accidental changes, not a security boundary: a client can turn it off again with settings_set.")))
				[
					SNew(STextBlock).Text(FText::FromString(TEXT("Read-only mode: refuse tools that change assets, the level, files or the editor")))
				]
			];
			TSharedRef<SWrapBox> Cats = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(14, 4));
			TMap<FString, int32> Counts;
			for (const FEWTool& T : FEWRegistry::Get().All()) { Counts.FindOrAdd(T.Category)++; }
			for (const FString& Cat : FEWRegistry::Get().Categories())
			{
				const bool bLocked = Cat == TEXT("Editor");
				Cats->AddSlot()
				[
					SNew(SCheckBox)
					.IsEnabled(!bLocked)
					.ToolTipText(FText::FromString(bLocked ? TEXT("Core tools (search, doctor, settings, logs) are always on.") : TEXT("Turn this group of tools on or off.")))
					.IsChecked_Lambda([Cat]() { return FEWRegistry::IsCategoryDisabled(Cat) ? ECheckBoxState::Unchecked : ECheckBoxState::Checked; })
					.OnCheckStateChanged_Lambda([Cat](ECheckBoxState State)
					{
						ChangeSettings([Cat, State](UEnginewrightSettings* S)
						{
							S->DisabledCategories.RemoveAll([&Cat](const FString& C) { return C.Equals(Cat, ESearchCase::IgnoreCase); });
							if (State != ECheckBoxState::Checked) { S->DisabledCategories.Add(Cat); }
						});
					})
					[
						SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("%s (%d)"), *Cat, Counts.FindRef(Cat))))
					]
				];
			}
			Body->AddSlot().AutoHeight().Padding(0, 4)[Cats];

			// Log level
			Body->AddSlot().AutoHeight()[Heading(TEXT("Output Log"))];
			TSharedRef<SHorizontalBox> Levels = SNew(SHorizontalBox);
			struct FLevel { EEnginewrightLogLevel Level; const TCHAR* Label; const TCHAR* Tip; };
			static const FLevel LevelList[] = {
				{ EEnginewrightLogLevel::Quiet, TEXT("Quiet"), TEXT("Start-up line and errors only.") },
				{ EEnginewrightLogLevel::Calls, TEXT("Calls"), TEXT("One line per tool call: name, result, time.") },
				{ EEnginewrightLogLevel::Verbose, TEXT("Verbose"), TEXT("One line per tool call with its arguments.") } };
			for (const FLevel& L : LevelList)
			{
				const EEnginewrightLogLevel Level = L.Level;
				Levels->AddSlot().AutoWidth().Padding(0, 0, 14, 0)
				[
					SNew(SCheckBox)
					.Style(FAppStyle::Get(), "RadioButton")
					.ToolTipText(FText::FromString(L.Tip))
					.IsChecked_Lambda([Level]() { return GetDefault<UEnginewrightSettings>()->LogLevel == Level ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([Level](ECheckBoxState) { ChangeSettings([Level](UEnginewrightSettings* S) { S->LogLevel = Level; }); })
					[
						SNew(STextBlock).Text(FText::FromString(L.Label))
					]
				];
			}
			Body->AddSlot().AutoHeight().Padding(0, 2)[Levels];

			// Recent calls
			Body->AddSlot().AutoHeight()[Heading(TEXT("Recent calls"))];
			Body->AddSlot().AutoHeight()[SAssignNew(CallList, SVerticalBox)];

			// Doctor + footer
			Body->AddSlot().AutoHeight()[Heading(TEXT("Health check"))];
			Body->AddSlot().AutoHeight().Padding(0, 2)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SButton).Text(FText::FromString(TEXT("Run doctor")))
					.OnClicked_Lambda([this]() { RunDoctor(); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton).Text(FText::FromString(TEXT("Open in Project Settings")))
					.OnClicked_Lambda([]()
					{
						if (ISettingsModule* S = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
						{
							S->ShowViewer(TEXT("Project"), TEXT("Plugins"), TEXT("Enginewright"));
						}
						return FReply::Handled();
					})
				]
			];
			Body->AddSlot().AutoHeight().Padding(0, 4)
			[
				SNew(STextBlock).AutoWrapText(true).Text_Lambda([this]() { return FText::FromString(DoctorText); })
			];
			Body->AddSlot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FLinearColor(0.95f, 0.8f, 0.3f))
				.Text_Lambda([this]() { return FText::FromString(NoteText); })
			];

			ChildSlot
			[
				SNew(SBorder).BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder")).Padding(12.f)
				[
					SNew(SScrollBox) + SScrollBox::Slot()[Body]
				]
			];
			Refresh(0.0, 0.f);
			RegisterActiveTimer(1.f, FWidgetActiveTimerDelegate::CreateSP(this, &SEWPanel::Refresh));
		}

	private:
		TSharedPtr<SVerticalBox> ClientList;
		TSharedPtr<SVerticalBox> CallList;
		bool bShowToken = false;
		FString NoteText;
		FString DoctorText = TEXT("Checks Play state, unsaved packages, Zen sessions and editor throttling.");

		static TSharedRef<SWidget> Heading(const TCHAR* Text)
		{
			return SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 3)
				[
					SNew(STextBlock).Text(FText::FromString(Text)).Font(FAppStyle::Get().GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 3)[SNew(SSeparator)];
		}

		void Note(const FString& Text) { NoteText = Text; }

		EActiveTimerReturnType Refresh(double, float)
		{
			const FEnginewrightModule* M = Module();
			ClientList->ClearChildren();
			const TArray<FEWClientSeen> Seen = M ? M->GetClients() : TArray<FEWClientSeen>();
			if (!Seen.Num())
			{
				ClientList->AddSlot().AutoHeight()[SNew(STextBlock).ColorAndOpacity(Grey).Text(FText::FromString(TEXT("No client yet. Copy a config above into your agent.")))];
			}
			for (const FEWClientSeen& C : Seen)
			{
				ClientList->AddSlot().AutoHeight().Padding(0, 1)
				[
					SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("%s %s  -  %d request(s), last %s"),
						*C.Name, *C.Version, C.Requests, *Ago(C.LastSeen))))
				];
			}

			CallList->ClearChildren();
			const TArray<FEWCallRecord>& Calls = FEWRegistry::Get().RecentCalls();
			if (!Calls.Num())
			{
				CallList->AddSlot().AutoHeight()[SNew(STextBlock).ColorAndOpacity(Grey).Text(FText::FromString(TEXT("No tool call yet.")))];
			}
			for (int32 i = Calls.Num() - 1, Shown = 0; i >= 0 && Shown < 15; --i, ++Shown)
			{
				const FEWCallRecord& C = Calls[i];
				CallList->AddSlot().AutoHeight().Padding(0, 1)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 10, 0)
					[
						SNew(STextBlock).ColorAndOpacity(Grey).Text(FText::FromString(C.Time.ToString(TEXT("%H:%M:%S"))))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 10, 0)
					[
						SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Mono", 9)).Text(FText::FromString(C.Tool))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 10, 0)
					[
						SNew(STextBlock).ColorAndOpacity(C.bOk ? Green : Red).Text(FText::FromString(C.bOk ? TEXT("ok") : C.ErrorCode))
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(STextBlock).ColorAndOpacity(Grey).Text(FText::FromString(FString::Printf(TEXT("%.0f ms"), C.Ms)))
					]
				];
			}
			return EActiveTimerReturnType::Continue;
		}

		void RunDoctor()
		{
			DoctorText = TEXT("Running...");
			TWeakPtr<SEWPanel> Weak = SharedThis(this);
			FEWRegistry::Get().CallAsync(TEXT("doctor"), MakeShared<FJsonObject>(), [Weak](FEWResult Res)
			{
				TSharedPtr<SEWPanel> Self = Weak.Pin();
				if (!Self.IsValid()) { return; }
				if (!Res.bOk)
				{
					Self->DoctorText = FString::Printf(TEXT("doctor failed: %s %s"), *Res.ErrorCode, *Res.Message);
					return;
				}
				FString T = Res.Warnings.Num() ? FString::Printf(TEXT("%d warning(s):"), Res.Warnings.Num()) : FString(TEXT("Healthy: no warnings."));
				for (const FString& W : Res.Warnings) { T += TEXT("\n- ") + W; }
				Self->DoctorText = T;
			});
		}
	};

	TSharedRef<SDockTab> SpawnPanel(const FSpawnTabArgs&)
	{
		return SNew(SDockTab).TabRole(ETabRole::NomadTab)[SNew(SEWPanel)];
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped Owner(MenuOwner);
		UToolMenu* Bar = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.StatusBar.ToolBar"));
		if (!Bar) { return; }
		FToolMenuSection& Section = Bar->AddSection(TEXT("Enginewright"), FText::GetEmpty(), FToolMenuInsert(NAME_None, EToolMenuInsertType::First));
		Section.AddEntry(FToolMenuEntry::InitWidget(TEXT("EnginewrightStatus"), SNew(SEWStatusIndicator), FText::GetEmpty(), true, false));
	}
}

namespace EWUI
{
	void Register()
	{
		if (!FSlateApplication::IsInitialized()) { return; } // Commandlets (-run=...) have no UI.
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(EWPanelTabId, FOnSpawnTab::CreateStatic(&SpawnPanel))
			.SetDisplayName(FText::FromString(TEXT("Enginewright")))
			.SetTooltipText(FText::FromString(TEXT("MCP server state, token, client setup, tool categories and recent calls.")))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
			.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Settings"));
		MenusHandle = UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateStatic(&RegisterMenus));
	}

	void Unregister()
	{
		if (!FSlateApplication::IsInitialized()) { return; }
		UToolMenus::UnRegisterStartupCallback(MenusHandle);
		UToolMenus::UnregisterOwner(MenuOwner);
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(EWPanelTabId);
	}
}
