#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMCPBridge, Log, All);

class FMCPBridgeServer;

class FUE_MCP_BridgeModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * The live bridge server, or null before StartupModule has created it and
	 * after ShutdownModule has destroyed it. Read by the toolbar status widget
	 * once a second; safe from the game thread.
	 */
	static FMCPBridgeServer* GetBridgeServer();
};
