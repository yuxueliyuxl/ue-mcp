#include "MCPToolbarStatus.h"

#include "BridgeServer.h"
#include "UE_MCP_BridgeModule.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "ToolMenus.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Types/WidgetActiveTimerDelegate.h"

#define LOCTEXT_NAMESPACE "MCPToolbarStatus"

namespace FMCPToolbarStatus
{
	namespace
	{
		const FName ToolbarSectionName(TEXT("UE_MCPBridge"));
		const FName ToolbarEntryName(TEXT("UE_MCPBridgeStatus"));
		bool bInstalled = false;
	}

	void Install()
	{
		if (bInstalled)
		{
			return;
		}
		bInstalled = true;

		if (!UToolMenus::IsToolMenuUIEnabled() || !UToolMenus::Get())
		{
			UE_LOG(LogMCPBridge, Warning,
				TEXT("[UE-MCP] ToolMenus unavailable - toolbar status indicator skipped"));
			return;
		}

		// The level editor's toolbar is composed of several slim strips;
		// "User" is the one third-party tools extend, sitting just left of
		// the settings gear. Extending it here works even if the level editor
		// has not registered the menu yet - ExtendMenu creates it.
		UToolMenu* UserToolbar = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.User"));
		if (!UserToolbar)
		{
			return;
		}

		FToolMenuSection& Section = UserToolbar->FindOrAddSection(
			ToolbarSectionName,
			LOCTEXT("SectionLabel", "UE-MCP Bridge"));
		Section.AddEntry(FToolMenuEntry::InitWidget(
			ToolbarEntryName,
			SNew(SMCPServerStatusWidget),
			LOCTEXT("EntryLabel", "UE-MCP Bridge"),
			true,  // bNoIndent
			false  // bSearchable
		));
	}

	void Remove()
	{
		if (!bInstalled)
		{
			return;
		}
		bInstalled = false;

		if (UObjectInitialized() && UToolMenus::IsToolMenuUIEnabled() && UToolMenus::Get())
		{
			UToolMenus::Get()->RemoveSection(TEXT("LevelEditor.LevelEditorToolBar.User"), ToolbarSectionName);
		}
	}
}

void SMCPServerStatusWidget::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "NoBorder")
		.ContentPadding(FMargin(6.f, 2.f))
		.OnClicked(this, &SMCPServerStatusWidget::OnClicked)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.f, 0.f, 5.f, 0.f))
				[
					SAssignNew(Dot, STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
					.Text(FText::FromString(TEXT("\u25CF")))
				]
			+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SAssignNew(StatusText, STextBlock)
					.Font(FAppStyle::GetFontStyle("NormalText"))
				]
		]
	];

	UpdateStatus();
	RegisterActiveTimer(1.0f,
		FWidgetActiveTimerDelegate::CreateSP(this, &SMCPServerStatusWidget::RefreshStatus));
}

EActiveTimerReturnType SMCPServerStatusWidget::RefreshStatus(double /*InCurrentTime*/, float /*InDeltaTime*/)
{
	UpdateStatus();
	return EActiveTimerReturnType::Continue;
}

void SMCPServerStatusWidget::UpdateStatus()
{
	const FMCPBridgeServer* Server = FUE_MCP_BridgeModule::GetBridgeServer();
	const bool bRunning = Server && Server->IsRunning();
	const int32 Clients = Server ? Server->GetHandshakenClientCount() : 0;

	FLinearColor DotColor;
	FText StatusLabel;

	if (!bRunning)
	{
		DotColor = FLinearColor(0.85f, 0.25f, 0.25f);
		StatusLabel = LOCTEXT("StatusError", "UE-MCP \u00b7 Error");
	}
	else if (Clients > 0)
	{
		DotColor = FLinearColor(0.25f, 0.85f, 0.30f);
		StatusLabel = FText::Format(
			LOCTEXT("StatusConnected", "UE-MCP \u00b7 Connected ({0})"),
			FText::AsNumber(Clients));
	}
	else
	{
		DotColor = FLinearColor(0.95f, 0.70f, 0.20f);
		StatusLabel = FText::Format(
			LOCTEXT("StatusListening", "UE-MCP \u00b7 Listening :{0}"),
			FText::AsNumber(Server->GetPort()));
	}

	Dot->SetColorAndOpacity(DotColor);
	StatusText->SetText(StatusLabel);

	const FString PortLine = Server
		? FString::Printf(TEXT("Port: %d (%s)"), Server->GetPort(), *Server->GetPortSource())
		: TEXT("Port: unknown");
	const FString StatusFile = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("UE_MCP_Bridge"), TEXT("status.json"));
	SetToolTipText(FText::FromString(FString::Printf(
		TEXT("UE-MCP Bridge\n%s\n%s\nStatus file: %s\n\nClick to copy connection info"),
		*StatusLabel.ToString(), *PortLine, *StatusFile)));
}

FReply SMCPServerStatusWidget::OnClicked()
{
	const FMCPBridgeServer* Server = FUE_MCP_BridgeModule::GetBridgeServer();
	if (Server)
	{
		FPlatformApplicationMisc::ClipboardCopy(*FString::Printf(
			TEXT("UE-MCP Bridge: port %d (%s), %d connected client(s)"),
			Server->GetPort(), *Server->GetPortSource(), Server->GetHandshakenClientCount()));
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
