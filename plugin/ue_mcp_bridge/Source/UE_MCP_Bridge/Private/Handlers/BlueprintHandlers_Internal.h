#pragma once

// Helpers shared between BlueprintHandlers.cpp and BlueprintHandlers_Graph.cpp
// after the file was split. Kept in Private/ because it is internal to the
// plugin - no downstream code is expected to include this.

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

class UBlueprint;
class UActorComponent;

// ── Graph selectors ─────────────────────────────────────────────────────────
//
// list_blueprint_graphs hands callers a `selector` they can pass back as
// graphName, and duplicate names (AnimBP "Transition", "Locomotion") get an
// index suffix. search_call_sites has to report the same selector for the same
// graph or the two disagree about how to address one graph, so the rule lives
// here rather than being restated per caller.

/** "Locomotion" when the name is unique, "Locomotion[3]" when it is not. */
inline FString MakeGraphSelector(const FString& Name, int32 DuplicateIndex, int32 DuplicateCount)
{
	return DuplicateCount > 1
		? FString::Printf(TEXT("%s[%d]"), *Name, DuplicateIndex)
		: Name;
}

/** How many graphs share each name, which is what decides whether a selector
 *  needs its index suffix. */
inline void CountGraphNames(const TArray<UEdGraph*>& Graphs, TMap<FString, int32>& OutNameCounts)
{
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph)
		{
			++OutNameCounts.FindOrAdd(Graph->GetName());
		}
	}
}

// ── Graph dump files ────────────────────────────────────────────────────────
//
// read_graph writes an oversized result to a file under Saved/UE_MCP rather
// than returning it inline. search_call_sites follows the same convention, so
// both use the same path builder and the same writer.

inline FString MakeDefaultGraphDumpPath(const FString& AssetPath, const FString& GraphName)
{
	const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PathHash = FString::Printf(TEXT("%08x"), GetTypeHash(AssetPath + TEXT(":") + GraphName));
	const FString BaseName = FPaths::MakeValidFileName(AssetName + TEXT("_") + GraphName + TEXT("_") + PathHash);
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UE_MCP"), TEXT("GraphDumps"), BaseName + TEXT(".json"));
}

inline bool WriteJsonObjectToFile(
	const TSharedPtr<FJsonObject>& JsonObject,
	const FString& RequestedPath,
	const FString& AssetPath,
	const FString& GraphName,
	FString& OutResolvedPath,
	FString& OutError)
{
	OutResolvedPath = RequestedPath.IsEmpty() ? MakeDefaultGraphDumpPath(AssetPath, GraphName) : RequestedPath;
	if (FPaths::IsRelative(OutResolvedPath))
	{
		OutResolvedPath = FPaths::Combine(FPaths::ProjectSavedDir(), OutResolvedPath);
	}

	const FString Directory = FPaths::GetPath(OutResolvedPath);
	if (!Directory.IsEmpty() && !IFileManager::Get().MakeDirectory(*Directory, true))
	{
		OutError = FString::Printf(TEXT("Failed to create dump directory: %s"), *Directory);
		return false;
	}

	FString JsonText;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
	if (!FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer))
	{
		OutError = TEXT("Failed to serialize graph JSON");
		return false;
	}

	if (!FFileHelper::SaveStringToFile(JsonText, *OutResolvedPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Failed to write graph dump: %s"), *OutResolvedPath);
		return false;
	}

	return true;
}

// Resolve the named component template on a blueprint, honouring inheritance.
// See definition in BlueprintHandlers_Graph.cpp for the full contract (bForWrite
// semantics, ICH-override creation on write, CDO fallback on read, etc.).
UActorComponent* ResolveComponentTemplate(
	UBlueprint* Blueprint,
	const FString& ComponentName,
	bool bForWrite,
	bool& bOutIsInherited,
	TArray<FString>& OutAvailable);

// #942: a level script Blueprint is not an asset of its own. It lives inside
// the map package at "<Map>.<Map>:PersistentLevel.<Map>", so every Blueprint
// action handed the umap path a caller actually has answered "Blueprint not
// found". FBlueprintHandlers::LoadBlueprint now resolves a World path to that
// object; the two helpers below carry the shared reporting around it, so read,
// list_graphs, read_graph and get_execution_flow behave identically.
//
// Builds the "not found" response for a failed Blueprint lookup. When the path
// names a World whose level script has never been created, the message says so
// and prints the object path, rather than claiming the Blueprint is missing.
TSharedPtr<FJsonValue> BlueprintNotFoundError(const FString& AssetPath);

// Record which object actually answered the request. A caller that passed a
// umap path gets back the level script Blueprint's object path, so the alias it
// used is visible rather than implied.
void AnnotateResolvedBlueprint(const TSharedPtr<FJsonObject>& Result, UBlueprint* Blueprint);
