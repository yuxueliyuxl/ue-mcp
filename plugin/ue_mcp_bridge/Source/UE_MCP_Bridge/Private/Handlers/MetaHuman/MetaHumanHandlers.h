#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"

// MetaHuman Creator over UMetaHumanCharacterEditorSubsystem (#1354). The
// MetaHumanCharacter plugin is optional and disabled by default, so every call
// goes through the subsystem's reflected UFUNCTIONs and fails with "MetaHuman
// plugin not available" when the plugin is off. No Build.cs dependency.
class FMetaHumanHandlers
{
public:
	static void RegisterHandlers(class FMCPHandlerRegistry& Registry);

private:
	static TSharedPtr<FJsonValue> Status(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> CreateCharacter(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> ReadCharacter(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> SetBody(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> SetWardrobe(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> RequestRig(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> RequestTextures(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> Assemble(const TSharedPtr<FJsonObject>& Params);
};
