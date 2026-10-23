#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MCPHandlerSpec.h"

namespace UEMCP
{
	/**
	 * Why Params break Spec, or empty when they keep it (#1282): an undeclared key, a parameter given under its
	 * name and an alias, a missing required parameter, a value its type, enum, range, fields, variants, forms or
	 * literal refuse, or an unmet choice. Keys compare case-sensitively. The server's contractViolation applies
	 * the same rules; the bridge runs this on every external handler registered with a contract, so a call that
	 * reaches the bridge without the server is held to the contract too. Run it on the parameters before aliases
	 * are resolved.
	 */
	UE_MCP_BRIDGE_API FString ContractViolation(const FMCPHandlerSpec& Spec, const TSharedPtr<FJsonObject>& Params);
}
