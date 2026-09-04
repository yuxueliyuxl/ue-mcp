#include "UE_MCP_BridgeModule.h"
#include "Modules/ModuleManager.h"
#include "BridgeServer.h"
#include "EngineStatusHooks.h"
#include "MCPEngineStatus.h"
#include "Toolbar/MCPToolbarStatus.h"
#include "Handlers/DialogHandlers.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "Containers/Ticker.h"

DEFINE_LOG_CATEGORY(LogMCPBridge);
IMPLEMENT_MODULE(FUE_MCP_BridgeModule, UE_MCP_Bridge)

static TSharedPtr<FMCPBridgeServer> G_BridgeServer;

FMCPBridgeServer* FUE_MCP_BridgeModule::GetBridgeServer()
{
	return G_BridgeServer.Get();
}

namespace
{
	/**
	 * #968: dialog policies supplied at launch, before anything can wedge.
	 *
	 * A modal raised during startup blocks the game thread, and while the
	 * dialog handlers can now clear one from outside, an agent that already
	 * knows the answer should not have to. `UE_MCP_DIALOG_POLICY` (or
	 * `-MCPDialogPolicy=`) is a semicolon separated list of `pattern=response`
	 * pairs, applied here at module startup so the very first prompt is already
	 * covered:
	 *
	 *     UE_MCP_DIALOG_POLICY="Restore=no;Would you like to rebuild=yes"
	 *
	 * The response words are exactly the ones set_dialog_policy accepts,
	 * because each entry is applied by dispatching set_dialog_policy itself
	 * rather than by a second copy of its parsing. An entry that is not a
	 * pattern=response pair is skipped and said out loud: a policy that
	 * silently does nothing is worse than no policy at all.
	 */
	FString ReadConfiguredDialogPolicy()
	{
		FString FromCommandLine;
		if (FParse::Value(FCommandLine::Get(), TEXT("MCPDialogPolicy="), FromCommandLine) && !FromCommandLine.IsEmpty())
		{
			return FromCommandLine;
		}
		return FPlatformMisc::GetEnvironmentVariable(TEXT("UE_MCP_DIALOG_POLICY"));
	}

	void ApplyConfiguredDialogPolicies(FMCPHandlerRegistry& Registry)
	{
		const FString Spec = ReadConfiguredDialogPolicy();
		if (Spec.IsEmpty())
		{
			return;
		}

		TArray<FString> Entries;
		Spec.ParseIntoArray(Entries, TEXT(";"), /*InCullEmpty*/ true);
		for (const FString& Entry : Entries)
		{
			// Split on the LAST '=' so a pattern may contain one.
			int32 Separator = INDEX_NONE;
			if (!Entry.FindLastChar(TEXT('='), Separator))
			{
				UE_LOG(LogMCPBridge, Warning,
					TEXT("[UE-MCP] Dialog policy entry '%s' has no 'pattern=response' separator and was ignored."), *Entry);
				continue;
			}
			const FString Pattern = Entry.Left(Separator).TrimStartAndEnd();
			const FString Response = Entry.Mid(Separator + 1).TrimStartAndEnd();
			if (Pattern.IsEmpty() || Response.IsEmpty())
			{
				UE_LOG(LogMCPBridge, Warning,
					TEXT("[UE-MCP] Dialog policy entry '%s' names an empty pattern or response and was ignored."), *Entry);
				continue;
			}

			TSharedPtr<FJsonObject> PolicyParams = MakeShared<FJsonObject>();
			PolicyParams->SetStringField(TEXT("pattern"), Pattern);
			PolicyParams->SetStringField(TEXT("response"), Response);
			Registry.ExecuteHandler(TEXT("set_dialog_policy"), PolicyParams);

			UE_LOG(LogMCPBridge, Log,
				TEXT("[UE-MCP] Startup dialog policy applied: '%s' answers '%s'"), *Pattern, *Response);
		}
	}
}

void FUE_MCP_BridgeModule::StartupModule()
{
	// Create and start bridge server. The base port is derived per-worktree
	// from the project root path, unless something pins it: -MCPPort,
	// UE_MCP_PORT, or `bridge.port` in the project's ue-mcp.yml layers, in the
	// order the client uses (#819). Deriving lets multiple checkouts run
	// side-by-side without colliding; the probe loop in Run() resolves the rare
	// clash and publishes the actual bound port to the per-project lockfile.
	const FMCPBridgePortChoice PortChoice = FMCPBridgeServer::ResolveConfiguredPort();
	G_BridgeServer = MakeShared<FMCPBridgeServer>(PortChoice.Port, PortChoice.Source, PortChoice.bPinned);

	// The snapshot has been publishing since PostConfigInit, from the
	// UE_MCP_BridgeStatus module. Now that Slate, the shader compiler and the
	// asset compiler exist, hand it the sensors that need them.
	FMCPEngineStatusHooks::Install();
	FMCPEngineStatus::Get().SetPhase(TEXT("bridge starting"));

	FDialogHandlers::InstallDialogHook();
	// Safety net: auto-decline overwrite dialogs to prevent game thread blocking.
	// Handlers should check for existing assets before creating, but if a dialog
	// slips through, decline it rather than blocking the game thread forever.
	FDialogHandlers::AddDefaultPolicy(TEXT("already exists"), EAppReturnType::No);
	FDialogHandlers::AddDefaultPolicy(TEXT("Overwrite"), EAppReturnType::No);
	// Safety-net for the editor's auto "save level / save unsaved" prompts.
	// When an agent session ends or the editor closes, these would otherwise
	// block the main thread waiting on a human. Default to "Discard".
	// (Agents that actually want to persist changes still call project(build)
	//  / level(save) / asset(save) explicitly.)
	FDialogHandlers::AddDefaultPolicy(TEXT("Save Changes"), EAppReturnType::No);
	FDialogHandlers::AddDefaultPolicy(TEXT("Save Content"), EAppReturnType::No);
	FDialogHandlers::AddDefaultPolicy(TEXT("Unsaved"), EAppReturnType::No);
	FDialogHandlers::AddDefaultPolicy(TEXT("Untitled"), EAppReturnType::No);
	FDialogHandlers::AddDefaultPolicy(TEXT("save your changes"), EAppReturnType::No);
	FDialogHandlers::AddDefaultPolicy(TEXT("save the level"), EAppReturnType::No);

	// #968: policies the launcher asked for, applied before the socket is even
	// listening, so a prompt raised during startup is answered rather than
	// waited on. Matching is first-registered-wins, so these sit behind the
	// safety nets above and add to them rather than reopening a save prompt
	// that already has a settled answer.
	ApplyConfiguredDialogPolicies(G_BridgeServer->GetHandlerRegistry());

	if (G_BridgeServer->Start())
	{
		UE_LOG(LogMCPBridge, Log, TEXT("[UE-MCP] Bridge server starting on base port %d (%s)"), PortChoice.Port, *PortChoice.Source);
	}
	else
	{
		UE_LOG(LogMCPBridge, Warning, TEXT("[UE-MCP] Failed to start bridge server"));
	}

	// The status indicator in the level editor toolbar reads the live server
	// counters, so it only appears once the server object exists.
	FMCPToolbarStatus::Install();

	// Defer the editor-ready signal until GEditor is available and has at least one world.
	// GetEditorWorldContext(false) can fail if no editor world context exists yet,
	// so we iterate all world contexts instead (#162).
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float) -> bool
		{
			if (!GEditor)
			{
				return true; // keep ticking - not ready yet
			}

			// Accept any world context (editor or PIE) as proof the editor is usable.
			bool bHasWorld = false;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.World())
				{
					bHasWorld = true;
					break;
				}
			}
			if (!bHasWorld)
			{
				return true; // keep ticking
			}

			if (G_BridgeServer.IsValid())
			{
				G_BridgeServer->GetGameThreadExecutor().SetEditorReady();
				UE_LOG(LogMCPBridge, Log, TEXT("[UE-MCP] Editor ready - accepting requests"));
			}
			FMCPEngineStatus::Get().SetPhase(TEXT("ready"));

			return false; // done
		})
	);
}

void FUE_MCP_BridgeModule::ShutdownModule()
{
	// Tear the toolbar widget down before the server object goes away: the
	// widget polls it once a second, and removing the menu entry destroys the
	// widget synchronously.
	FMCPToolbarStatus::Remove();

	FDialogHandlers::RemoveDialogHook();
	// The snapshot itself outlives this module (its own module owns it and
	// keeps publishing until PostConfigInit teardown); only the Slate and
	// Engine sensors go away with us.
	FMCPEngineStatusHooks::Remove();

	if (G_BridgeServer.IsValid())
	{
		G_BridgeServer->Shutdown();
		G_BridgeServer.Reset();
		UE_LOG(LogMCPBridge, Log, TEXT("[UE-MCP] Bridge server stopped"));
	}
}
