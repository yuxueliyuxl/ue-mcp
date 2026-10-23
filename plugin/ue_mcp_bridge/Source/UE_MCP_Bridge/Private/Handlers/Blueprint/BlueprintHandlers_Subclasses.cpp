// Translation-unit partition of FBlueprintHandlers, like the other
// BlueprintHandlers_*.cpp files. Registration stays in BlueprintHandlers.cpp.

#include "BlueprintHandlers.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"
#include "Engine/Blueprint.h"
#include "Blueprint/BlueprintSupport.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ─── #1211 list_subclasses ──────────────────────────────────────────────────
// Blueprints deriving from a class, read from the Asset Registry's
// GeneratedClass/ParentClass/NativeParentClass tags so nothing is loaded.
// Parent chains are walked over every Blueprint, so a parent outside the
// requested directory still links its children to the class.
TSharedPtr<FJsonValue> FBlueprintHandlers::ListSubclasses(const TSharedPtr<FJsonObject>& Params)
{
	// Every parameter is read before anything can fail (#1057).
	FString ClassName;
	TSharedPtr<FJsonValue> ClassErr = RequireString(Params, TEXT("className"), ClassName);
	FString Directory = OptionalString(Params, TEXT("directory"), TEXT("/Game"));
	const bool bTransitive = OptionalBool(Params, TEXT("includeTransitive"), true);
	const int32 Limit = FMath::Clamp(OptionalInt(Params, TEXT("limit"), 2000), 1, 20000);
	if (ClassErr) return ClassErr;

	UClass* Target = MCPResolveClass(ClassName);
	if (!Target) return MCPClassNotFoundError(ClassName);
	const FString TargetPath = Target->GetPathName();
	const bool bTargetNative = Target->HasAnyClassFlags(CLASS_Native);

	Directory.TrimStartAndEndInline();
	while (Directory.Len() > 1 && Directory.EndsWith(TEXT("/"))) Directory.LeftChopInline(1);
	if (!Directory.StartsWith(TEXT("/")))
	{
		return MCPError(FString::Printf(TEXT("'directory' must be a mount-rooted content path such as /Game, got '%s'"), *Directory));
	}

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	// A sweep during the initial scan would miss every asset not reached yet.
	const bool bWaited = Registry.IsLoadingAssets();
	if (bWaited) Registry.WaitForCompletion();
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	TArray<FAssetData> Blueprints;
	Registry.GetAssets(Filter, Blueprints);

	auto TagPath = [](const FAssetData& Asset, const FName Tag) -> FString
	{
		FString Value;
		if (!Asset.GetTagValue(Tag, Value) || Value.IsEmpty() || Value == TEXT("None")) return FString();
		return FPackageName::ExportTextPathToObjectPath(Value);
	};

	struct FEntry { int32 Asset; FString Parent; FString NativeParent; };
	TArray<FEntry> Entries;
	TMap<FString, int32> ByGeneratedClass;
	for (int32 i = 0; i < Blueprints.Num(); ++i)
	{
		FEntry E{ i, TagPath(Blueprints[i], FBlueprintTags::ParentClassPath), TagPath(Blueprints[i], FBlueprintTags::NativeParentClassPath) };
		const FString Generated = TagPath(Blueprints[i], FBlueprintTags::GeneratedClassPath);
		const int32 Index = Entries.Add(E);
		if (!Generated.IsEmpty()) ByGeneratedClass.Add(Generated, Index);
	}

	const FString DirPrefix = Directory == TEXT("/") ? Directory : Directory + TEXT("/");
	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 Matched = 0;
	for (const FEntry& E : Entries)
	{
		const FAssetData& Asset = Blueprints[E.Asset];
		const FString PackageName = Asset.PackageName.ToString();
		if (Directory != TEXT("/") && !PackageName.StartsWith(DirPrefix)) continue;

		// Walk Blueprint parents, then native supers, counting the distance.
		int32 Depth = 0;
		FString Cursor = E.Parent;
		for (int32 Guard = 0; Guard < 128 && !Cursor.IsEmpty(); ++Guard)
		{
			++Depth;
			if (Cursor == TargetPath) break;
			if (const int32* Next = ByGeneratedClass.Find(Cursor))
			{
				Cursor = Entries[*Next].Parent;
				continue;
			}
			UClass* Native = bTargetNative ? FindObject<UClass>(nullptr, *Cursor) : nullptr;
			Cursor.Reset();
			for (UClass* Super = Native ? Native->GetSuperClass() : nullptr; Super; Super = Super->GetSuperClass())
			{
				++Depth;
				if (Super == Target) { Cursor = TargetPath; break; }
			}
			break;
		}
		if (Cursor != TargetPath) continue;
		if (!bTransitive && Depth != 1) continue;

		++Matched;
		if (Rows.Num() >= Limit) continue;
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		Row->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Row->SetStringField(TEXT("assetClass"), Asset.AssetClassPath.GetAssetName().ToString());
		Row->SetStringField(TEXT("parentClass"), E.Parent);
		Row->SetStringField(TEXT("nativeParentClass"), E.NativeParent);
		Row->SetNumberField(TEXT("depth"), Depth);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Rows.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		return A->AsObject()->GetStringField(TEXT("path")) < B->AsObject()->GetStringField(TEXT("path"));
	});

	TSharedPtr<FJsonObject> Result = MCPSuccess();
	Result->SetStringField(TEXT("className"), TargetPath);
	Result->SetStringField(TEXT("directory"), Directory);
	Result->SetBoolField(TEXT("includeTransitive"), bTransitive);
	Result->SetArrayField(TEXT("subclasses"), Rows);
	Result->SetNumberField(TEXT("count"), Matched);
	if (Matched > Rows.Num())
	{
		Result->SetBoolField(TEXT("truncated"), true);
		Result->SetNumberField(TEXT("limit"), Limit);
	}
	Result->SetNumberField(TEXT("blueprintsScanned"), Blueprints.Num());
	Result->SetBoolField(TEXT("waitedForAssetRegistryScan"), bWaited);
	return MCPResult(Result);
}
