#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"

// Datasmith scene inspect, import, reimport and level metadata queries (#1383).
// The Datasmith plugins are optional, so every call goes through reflection on
// their scripting classes and fails with "Datasmith plugin not available" when
// they are not loaded. No Build.cs dependency.
class FDatasmithHandlers
{
public:
	static void RegisterHandlers(class FMCPHandlerRegistry& Registry);

private:
	static TSharedPtr<FJsonValue> Status(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> Inspect(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> Import(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> Reimport(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> QueryActors(const TSharedPtr<FJsonObject>& Params);
};
