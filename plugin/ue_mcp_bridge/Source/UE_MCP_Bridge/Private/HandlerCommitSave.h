#pragma once

// Save through the editor's own dirty-package path (#1156). A direct
// UPackage::Save of a deleted World Partition actor's external package fails;
// the editor path deletes that package's file instead. Shared by level(save)
// and editor(save_dirty), so it lives here rather than in either .cpp.

#include "CoreMinimal.h"
#include "HandlerUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "ObjectTools.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/** Take the dirty, emptied external actor/object packages out of Packages and delete their files, as the
 *  editor's own save does; a direct save of one fails (#1301). One row per package goes to OutDeleted
 *  (file removed, or never on disk) or OutFailed (file still there). */
inline void MCPCommitEmptyExternalPackages(
	TArray<UPackage*>& Packages, TArray<TSharedPtr<FJsonValue>>& OutDeleted, TArray<TSharedPtr<FJsonValue>>& OutFailed)
{
	struct FEmptyEntry { FString Package; FString File; bool bExisted = false; };
	TArray<UPackage*> Empty;
	TArray<FEmptyEntry> Entries;
	for (int32 Index = Packages.Num() - 1; Index >= 0; --Index)
	{
		UPackage* Package = Packages[Index];
		if (!Package || !Package->IsDirty()) continue;
		const FString Name = Package->GetName();
		if (!Name.Contains(TEXT("/__ExternalActors__/")) && !Name.Contains(TEXT("/__ExternalObjects__/"))) continue;
		if (!UPackage::IsEmptyPackage(Package)) continue;
		FEmptyEntry Entry;
		Entry.Package = Name;
		ResolvePackageFileName(Package, Entry.File);
		Entry.bExisted = !Entry.File.IsEmpty() && IFileManager::Get().FileExists(*Entry.File);
		Entries.Add(Entry);
		Empty.Add(Package);
		Packages.RemoveAt(Index);
	}
	if (Empty.IsEmpty()) return;

	ObjectTools::CleanupAfterSuccessfulDelete(Empty, /*bPerformReferenceCheck*/ true);

	for (const FEmptyEntry& Entry : Entries)
	{
		const bool bExistsNow = !Entry.File.IsEmpty() && IFileManager::Get().FileExists(*Entry.File);
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("package"), Entry.Package);
		if (!Entry.File.IsEmpty()) Row->SetStringField(TEXT("file"), FPaths::ConvertRelativePathToFull(Entry.File));
		Row->SetBoolField(TEXT("emptyPackage"), true);
		if (bExistsNow)
		{
			Row->SetStringField(TEXT("error"), TEXT("The emptied package's file could not be deleted; it may be read-only or held by source control."));
			OutFailed.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}
		Row->SetBoolField(TEXT("deleted"), Entry.bExisted);
		if (!Entry.bExisted) Row->SetBoolField(TEXT("neverOnDisk"), true);
		OutDeleted.Add(MakeShared<FJsonValueObject>(Row));
	}
}

/** Save every dirty map and/or content package with
 *  UEditorLoadingAndSavingUtils::SaveDirtyPackages, and report each package
 *  as written, deleted (its file existed before and is gone now), stillDirty
 *  or discarded (never on disk and not written). */
inline TSharedPtr<FJsonObject> MCPSaveDirtyCommittingDeletes(bool bSaveMaps, bool bSaveContent)
{
	TArray<UPackage*> Dirty;
	if (bSaveContent) FEditorFileUtils::GetDirtyContentPackages(Dirty);
	if (bSaveMaps) FEditorFileUtils::GetDirtyWorldPackages(Dirty);

	struct FMCPCommitEntry
	{
		FString Package;
		FString File;
		bool bExisted = false;
		bool bEmpty = false;
	};
	TArray<FMCPCommitEntry> Entries;
	TSet<FString> Seen;
	for (UPackage* Package : Dirty)
	{
		if (!Package || !Package->IsDirty()) continue;
		const FString Name = Package->GetName();
		if (Seen.Contains(Name)) continue;
		Seen.Add(Name);
		FMCPCommitEntry Entry;
		Entry.Package = Name;
		ResolvePackageFileName(Package, Entry.File);
		Entry.bExisted = !Entry.File.IsEmpty() && IFileManager::Get().FileExists(*Entry.File);
		Entry.bEmpty = UPackage::IsEmptyPackage(Package);
		Entries.Add(Entry);
	}

	FMCPSaveDiagnostics Diagnostics;
	const bool bOk = UEditorLoadingAndSavingUtils::SaveDirtyPackages(bSaveMaps, bSaveContent);

	TArray<TSharedPtr<FJsonValue>> Written, Deleted, StillDirty, Discarded;
	for (const FMCPCommitEntry& Entry : Entries)
	{
		UPackage* Package = FindPackage(nullptr, *Entry.Package);
		const bool bDirty = Package && Package->IsDirty();
		const bool bExistsNow = !Entry.File.IsEmpty() && IFileManager::Get().FileExists(*Entry.File);

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("package"), Entry.Package);
		if (!Entry.File.IsEmpty()) Row->SetStringField(TEXT("file"), FPaths::ConvertRelativePathToFull(Entry.File));
		if (Entry.bEmpty) Row->SetBoolField(TEXT("emptyPackage"), true);
		const TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueObject>(Row);

		if (Entry.bExisted && !bExistsNow) Deleted.Add(Value);
		else if (bDirty)                   StillDirty.Add(Value);
		else if (bExistsNow)               Written.Add(Value);
		else                               Discarded.Add(Value);
	}

	TSharedPtr<FJsonObject> Result = MCPSuccess();
	Result->SetBoolField(TEXT("commitDeletes"), true);
	Result->SetNumberField(TEXT("dirtyCount"), Entries.Num());
	Result->SetNumberField(TEXT("writtenCount"), Written.Num());
	Result->SetNumberField(TEXT("deletedCount"), Deleted.Num());
	Result->SetNumberField(TEXT("failedCount"), StillDirty.Num());
	Result->SetArrayField(TEXT("written"), Written);
	Result->SetArrayField(TEXT("deleted"), Deleted);
	if (Discarded.Num() > 0) Result->SetArrayField(TEXT("discarded"), Discarded);
	Result->SetBoolField(TEXT("savedAll"), bOk && StillDirty.Num() == 0);
	Result->SetBoolField(TEXT("changed"), Written.Num() + Deleted.Num() > 0);
	if (StillDirty.Num() > 0)
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetArrayField(TEXT("stillDirty"), StillDirty);
		Result->SetStringField(TEXT("error"), FString::Printf(
			TEXT("%d package(s) are still dirty after the save; saveDiagnostics carries the engine's reason when it logged one."),
			StillDirty.Num()));
		MCPAttachSaveDiagnostics(Result, Diagnostics);
	}
	MCPSetNoRollback(Result,
		TEXT("Written packages were overwritten on disk and deleted actor packages were removed from it. The bridge keeps no copy of either; source control holds the previous revision."));
	return Result;
}
