#pragma once

// Editor-state helpers shared by more than one handler translation unit.
//
// The module is compiled as a unity build, so a file-local helper copied into a
// second .cpp becomes a redefinition the moment UBT groups the two files into
// one blob (error C2084), and the grouping is not stable across machines.
// Anything two handlers both need lives here, in a named namespace, as an
// inline function. It is a Private header rather than Public/HandlerUtils.h
// because it pulls in the editor file utilities, and HandlerUtils.h is also
// included by UE_MCP_BridgeStatus, which does not link UnrealEd.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace MCPEditorState
{
	/**
	 * Long package names of every dirty content or map package in the editor.
	 *
	 * Two handlers need this for the same reason: an operation that temporarily
	 * opens a different map has to refuse when the editor already has unsaved
	 * work, because loading another level either discards it or raises a modal
	 * save prompt the bridge cannot answer.
	 *
	 * `/Script/` packages are always dirty-ish bookkeeping and are excluded.
	 */
	inline void CollectDirtyEditorPackageNames(TArray<FString>& OutPackageNames)
	{
		OutPackageNames.Reset();
		TArray<UPackage*> DirtyPackages;
		FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);
		FEditorFileUtils::GetDirtyWorldPackages(DirtyPackages);
		for (UPackage* Package : DirtyPackages)
		{
			if (!Package || !Package->IsDirty())
			{
				continue;
			}

			const FString PackageName = Package->GetName();
			if (PackageName.StartsWith(TEXT("/Script/")))
			{
				continue;
			}

			OutPackageNames.AddUnique(PackageName);
		}
		OutPackageNames.Sort();
	}

	/** Long package name of the map currently open in the editor, or empty. */
	inline FString CurrentEditorLevelPackageName()
	{
		if (!GEditor)
		{
			return FString();
		}
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World || !World->GetOutermost())
		{
			return FString();
		}
		return World->GetOutermost()->GetName();
	}

	/** Is this long package name an existing .umap on disk? */
	inline bool IsExistingMapPackage(const FString& LevelPath)
	{
		if (LevelPath.IsEmpty() || !FPackageName::IsValidLongPackageName(LevelPath))
		{
			return false;
		}
		FString Filename;
		return FPackageName::DoesPackageExist(LevelPath, &Filename) &&
			Filename.EndsWith(FPackageName::GetMapPackageExtension(), ESearchCase::IgnoreCase);
	}
}
