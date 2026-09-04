// foliage(batch_set_settings_where) (#988).
//
// The reporter needed include_in_hlod=false on every FoliageType whose cull
// distance was set, which was 36 of 201. foliage(set_settings) is one asset per
// call, and there was no way to express the predicate at all, so the match had
// to be computed client-side from 201 separate reads and then written back with
// 36 more calls.
//
// The predicate runs here, over the CURRENT value of a property on each
// candidate, using the same operator vocabulary as level(query_components) and
// asset(bulk_read_properties). Translation-unit partition of FFoliageHandlers;
// registration stays in FoliageHandlers.cpp.

#include "FoliageHandlers.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FoliageType.h"
#include "HandlerJsonProperty.h"
#include "HandlerQuery.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"
#include "InstancedFoliageActor.h"
#include "UObject/Package.h"

namespace
{
	constexpr int32 MCPFoliageBatchMaxTypes = 2000;
	constexpr int32 MCPFoliageBatchMaxPredicates = 16;
	constexpr int32 MCPFoliageBatchMaxSettings = 32;
	constexpr int32 MCPFoliageBatchMaxResultRows = 500;

	/** One candidate, with everything needed to report it whether or not it
	 *  ends up matching. */
	struct FMCPFoliageBatchCandidate
	{
		UFoliageType* Type = nullptr;
		FString Path;
		FString Name;
		FString Source;
	};

	void MCPFoliageBatchAddCandidate(
		TArray<FMCPFoliageBatchCandidate>& Candidates,
		TSet<FString>& Seen,
		UFoliageType* Type,
		const TCHAR* Source)
	{
		if (!Type)
		{
			return;
		}
		const FString Path = Type->GetPathName();
		if (Seen.Contains(Path))
		{
			return;
		}
		Seen.Add(Path);
		FMCPFoliageBatchCandidate Candidate;
		Candidate.Type = Type;
		Candidate.Path = Path;
		Candidate.Name = Type->GetName();
		Candidate.Source = Source;
		Candidates.Add(MoveTemp(Candidate));
	}
}

TSharedPtr<FJsonValue> FFoliageHandlers::BatchSetFoliageSettingsWhere(const TSharedPtr<FJsonObject>& Params)
{
	MCP_CHECK_GAME_THREAD();

	// ── What to write ───────────────────────────────────────────────────────
	const TSharedPtr<FJsonObject>* SettingsObject = nullptr;
	if (!Params->TryGetObjectField(TEXT("settings"), SettingsObject) ||
		!SettingsObject || !(*SettingsObject).IsValid() || (*SettingsObject)->Values.Num() == 0)
	{
		return MCPError(TEXT("Missing 'settings' (an object of propertyName -> value; dotted paths such as 'CullDistance.Max' are supported)"));
	}
	if ((*SettingsObject)->Values.Num() > MCPFoliageBatchMaxSettings)
	{
		return MCPError(FString::Printf(
			TEXT("'settings' exceeds the maximum of %d entries"), MCPFoliageBatchMaxSettings));
	}

	// ── The predicate, which is the point of the action ─────────────────────
	TArray<MCPQuery::FPredicate> Predicates;
	{
		const TArray<TSharedPtr<FJsonValue>>* WhereValues = nullptr;
		Params->TryGetArrayField(TEXT("where"), WhereValues);
		FString ParseError;
		if (!MCPQuery::ParsePredicates(WhereValues, MCPFoliageBatchMaxPredicates, Predicates, ParseError))
		{
			return MCPError(ParseError);
		}
	}
	if (Predicates.IsEmpty())
	{
		// Without a predicate this is "write these settings to every foliage
		// type in the project", which is not what anybody means by a
		// conditional batch and is not worth guessing at.
		return MCPError(TEXT("Missing 'where' ([{field, op, value}] over the current property values, e.g. [{field:'props.CullDistance.Max', op:'gt', value:0}]). Use foliage(set_settings) for a single known asset."));
	}
	const FString WhereMode = OptionalString(Params, TEXT("whereMode"), TEXT("all")).ToLower();
	if (WhereMode != TEXT("all") && WhereMode != TEXT("any"))
	{
		return MCPError(TEXT("'whereMode' must be either 'all' or 'any'"));
	}

	// Properties the predicate reads. Derived from the predicate fields so a
	// caller does not have to list them twice, plus anything explicitly asked
	// for so the report can show context it does not filter on.
	TArray<FString> ReadPaths;
	for (MCPQuery::FPredicate& Predicate : Predicates)
	{
		if (Predicate.Field.StartsWith(TEXT("props.")))
		{
			ReadPaths.AddUnique(Predicate.Field.RightChop(6));
			continue;
		}
		// A bare field name is read as a property too, so
		// {field:'CullDistance.Max'} and {field:'props.CullDistance.Max'} mean
		// the same thing rather than one of them silently matching nothing.
		ReadPaths.AddUnique(Predicate.Field);
		Predicate.Field = FString::Printf(TEXT("props.%s"), *Predicate.Field);
	}
	{
		const TArray<TSharedPtr<FJsonValue>>* ExtraValues = nullptr;
		if (Params->TryGetArrayField(TEXT("propertyNames"), ExtraValues) && ExtraValues)
		{
			for (const FString& Path : JsonArrayToStringList(ExtraValues))
			{
				ReadPaths.AddUnique(Path);
			}
		}
	}

	const bool bDryRun = OptionalBool(Params, TEXT("dryRun"), true);
	const bool bSave = OptionalBool(Params, TEXT("save"), true);
	const int32 MaxTypes = FMath::Clamp(
		OptionalInt(Params, TEXT("maxTypes"), MCPFoliageBatchMaxTypes), 1, MCPFoliageBatchMaxTypes);

	// ── Candidates ──────────────────────────────────────────────────────────
	TArray<FMCPFoliageBatchCandidate> Candidates;
	TSet<FString> Seen;

	TArray<FString> ExplicitPaths;
	{
		const TArray<TSharedPtr<FJsonValue>>* PathValues = nullptr;
		if (Params->TryGetArrayField(TEXT("foliageTypePaths"), PathValues) && PathValues)
		{
			ExplicitPaths = JsonArrayToStringList(PathValues);
		}
	}
	TArray<FString> UnloadablePaths;
	for (const FString& Path : ExplicitPaths)
	{
		UFoliageType* Type = LoadObject<UFoliageType>(nullptr, *Path);
		if (!Type)
		{
			UnloadablePaths.Add(Path);
			continue;
		}
		MCPFoliageBatchAddCandidate(Candidates, Seen, Type, TEXT("explicit"));
	}

	const FString Directory = OptionalString(Params, TEXT("directory"));
	const bool bFromLevel = OptionalBool(Params, TEXT("fromLevel"), false);
	if (ExplicitPaths.IsEmpty() && Directory.IsEmpty() && !bFromLevel)
	{
		return MCPError(TEXT("Pass one of 'foliageTypePaths' (explicit), 'directory' (scan FoliageType assets) or fromLevel=true (types placed in the open level)"));
	}

	if (!Directory.IsEmpty())
	{
		FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		FARFilter Filter;
		Filter.PackagePaths.Add(FName(*Directory));
		Filter.bRecursivePaths = OptionalBool(Params, TEXT("recursive"), true);
		Filter.ClassPaths.Add(UFoliageType::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;

		TArray<FAssetData> Found;
		AssetRegistry.GetAssets(Filter, Found);
		for (const FAssetData& Data : Found)
		{
			if (Candidates.Num() >= MaxTypes) break;
			UFoliageType* Type = Cast<UFoliageType>(Data.GetAsset());
			if (!Type)
			{
				UnloadablePaths.Add(Data.GetObjectPathString());
				continue;
			}
			MCPFoliageBatchAddCandidate(Candidates, Seen, Type, TEXT("directory"));
		}
	}

	if (bFromLevel)
	{
		if (UWorld* World = GetEditorWorld())
		{
			for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
			{
				AInstancedFoliageActor* FoliageActor = *It;
				if (!FoliageActor) continue;
				for (const auto& Pair : FoliageActor->GetFoliageInfos())
				{
					if (Candidates.Num() >= MaxTypes) break;
					MCPFoliageBatchAddCandidate(Candidates, Seen, Pair.Key, TEXT("level"));
				}
			}
		}
	}

	Candidates.Sort([](const FMCPFoliageBatchCandidate& A, const FMCPFoliageBatchCandidate& B)
	{
		return A.Path < B.Path;
	});

	// ── Evaluate and write ──────────────────────────────────────────────────
	TArray<TSharedPtr<FJsonValue>> Rows;
	TArray<FString> MatchedPaths;
	TArray<FString> SavedPaths;
	int32 Matched = 0;
	int32 Updated = 0;
	int32 Failed = 0;
	int32 SaveFailures = 0;

	for (const FMCPFoliageBatchCandidate& Candidate : Candidates)
	{
		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		TArray<FString> MissingProperties;
		for (const FString& Path : ReadPaths)
		{
			FString ResolvedType;
			const TSharedPtr<FJsonValue> Value =
				MCPQuery::ReadDottedProperty(Candidate.Type, Path, ResolvedType);
			if (!Value.IsValid())
			{
				MissingProperties.Add(Path);
				continue;
			}
			Props->SetField(Path, Value);
		}

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("foliageTypePath"), Candidate.Path);
		Row->SetStringField(TEXT("name"), Candidate.Name);
		Row->SetStringField(TEXT("className"), Candidate.Type->GetClass()->GetName());
		Row->SetStringField(TEXT("source"), Candidate.Source);
		Row->SetObjectField(TEXT("props"), Props);
		if (MissingProperties.Num() > 0)
		{
			// A property the predicate reads that this class does not have is
			// reported rather than treated as a non-match, because "does not
			// apply" and "did not match" are different answers.
			Row->SetArrayField(TEXT("missingProperties"), MCPStringListToJson(MissingProperties));
		}

		if (!MCPQuery::EvaluateAll(Predicates, Row, WhereMode == TEXT("all")))
		{
			continue;
		}
		++Matched;
		MatchedPaths.Add(Candidate.Path);

		TArray<TSharedPtr<FJsonValue>> SettingRows;
		bool bTypeOk = true;
		bool bTypeChanged = false;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*SettingsObject)->Values)
		{
			TSharedPtr<FJsonObject> SettingRow = MakeShared<FJsonObject>();
			SettingRow->SetStringField(TEXT("propertyName"), Pair.Key);

			FString ResolvedType;
			const TSharedPtr<FJsonValue> Before =
				MCPQuery::ReadDottedProperty(Candidate.Type, Pair.Key, ResolvedType);
			if (Before.IsValid())
			{
				SettingRow->SetField(TEXT("previousValue"), Before);
			}

			if (bDryRun)
			{
				// The preview still resolves the path, so an unwritable
				// property is reported now rather than after the commit.
				SettingRow->SetBoolField(TEXT("ok"), Before.IsValid());
				if (!Before.IsValid())
				{
					SettingRow->SetStringField(TEXT("error"), FString::Printf(
						TEXT("Property '%s' does not resolve on %s"),
						*Pair.Key, *Candidate.Type->GetClass()->GetName()));
					bTypeOk = false;
				}
				SettingRows.Add(MakeShared<FJsonValueObject>(SettingRow));
				continue;
			}

			FString Error;
			const bool bOk = MCPJsonProperty::SetDottedPropertyFromJson(
				Candidate.Type, Pair.Key, Pair.Value, Error);
			SettingRow->SetBoolField(TEXT("ok"), bOk);
			if (!bOk)
			{
				SettingRow->SetStringField(TEXT("error"), Error);
				bTypeOk = false;
			}
			else
			{
				FString AfterType;
				const TSharedPtr<FJsonValue> After =
					MCPQuery::ReadDottedProperty(Candidate.Type, Pair.Key, AfterType);
				if (After.IsValid())
				{
					SettingRow->SetField(TEXT("value"), After);
					// Read back rather than echoing the request, so a value the
					// property coerced is visible instead of assumed.
					if (MCPQuery::ValueKey(Before) != MCPQuery::ValueKey(After))
					{
						bTypeChanged = true;
					}
				}
			}
			SettingRows.Add(MakeShared<FJsonValueObject>(SettingRow));
		}

		if (!bDryRun && bTypeOk)
		{
			Candidate.Type->MarkPackageDirty();
			++Updated;
			if (bSave)
			{
				if (SaveAssetPackage(Candidate.Type))
				{
					SavedPaths.Add(Candidate.Path);
				}
				else
				{
					++SaveFailures;
				}
			}
		}
		if (!bTypeOk) ++Failed;

		if (Rows.Num() < MCPFoliageBatchMaxResultRows)
		{
			Row->SetBoolField(TEXT("ok"), bTypeOk);
			Row->SetBoolField(TEXT("changed"), !bDryRun && bTypeChanged);
			Row->SetArrayField(TEXT("settings"), SettingRows);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
	}

	auto Result = MCPSuccess();
	Result->SetBoolField(TEXT("success"), Failed == 0 && UnloadablePaths.IsEmpty());
	if (Failed > 0)
	{
		Result->SetStringField(TEXT("error"), FString::Printf(
			TEXT("%d of %d matched foliage types had a setting that did not apply; see results[]."),
			Failed, Matched));
	}
	else if (!UnloadablePaths.IsEmpty())
	{
		Result->SetStringField(TEXT("error"), FString::Printf(
			TEXT("%d requested path(s) could not be loaded as a FoliageType; see unloadablePaths."),
			UnloadablePaths.Num()));
	}
	Result->SetBoolField(TEXT("dryRun"), bDryRun);
	Result->SetNumberField(TEXT("scanned"), Candidates.Num());
	Result->SetNumberField(TEXT("matched"), Matched);
	Result->SetNumberField(bDryRun ? TEXT("wouldUpdate") : TEXT("updated"), bDryRun ? Matched : Updated);
	Result->SetNumberField(TEXT("failed"), Failed);
	Result->SetArrayField(TEXT("matchedPaths"), MCPStringListToJson(MatchedPaths));
	Result->SetArrayField(TEXT("unloadablePaths"), MCPStringListToJson(UnloadablePaths));
	Result->SetNumberField(TEXT("returnedResults"), Rows.Num());
	Result->SetBoolField(TEXT("resultsTruncated"), Rows.Num() < Matched);
	Result->SetArrayField(TEXT("results"), Rows);

	if (Candidates.IsEmpty())
	{
		Result->SetStringField(TEXT("zeroMatchNote"),
			TEXT("No FoliageType was found to test. Check 'directory' against where the assets actually live, or pass fromLevel=true to take the types placed in the open level."));
	}
	else if (Matched == 0)
	{
		Result->SetStringField(TEXT("zeroMatchNote"), FString::Printf(
			TEXT("%d foliage types were read and none matched the predicate. Note that a numeric comparison needs a numeric property: CullDistance is an interval, so the field is CullDistance.Max rather than CullDistance."),
			Candidates.Num()));
	}

	if (!bDryRun)
	{
		Result->SetNumberField(TEXT("saved"), SavedPaths.Num());
		Result->SetNumberField(TEXT("saveFailures"), SaveFailures);
		Result->SetArrayField(TEXT("savedPaths"), MCPStringListToJson(SavedPaths));
		if (Updated > 0)
		{
			MCPSetUpdated(Result);
		}
		if (!bSave && Updated > 0)
		{
			Result->SetStringField(TEXT("saveNote"),
				TEXT("save=false: the FoliageType packages are dirty and NOT saved."));
		}
	}
	else
	{
		Result->SetStringField(TEXT("dryRunNote"),
			TEXT("dryRun defaults to TRUE for this action because it writes to many assets at once. Pass dryRun=false to commit."));
	}
	return MCPResult(Result);
}
