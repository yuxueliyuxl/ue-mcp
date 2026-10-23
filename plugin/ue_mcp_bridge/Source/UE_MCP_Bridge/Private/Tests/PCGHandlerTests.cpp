// PCG handlers through the registry: graph folder, spawner selector ownership,
// volume transform and the run record. Graphs live on a private mount and
// actors in a disposable world, never the open project or map.
#if WITH_DEV_AUTOMATION_TESTS

#include "HandlerRegistry.h"
#include "HandlerUtils.h"
#include "Handlers/PCG/PCGHandlers.h"
#include "Tests/MCPScopedTestMount.h"
#include "Editor.h"
#include "Elements/PCGAddTag.h"
#include "PCGComponent.h"
#include "PCGPin.h"
#include "PCGSubgraph.h"
#include "HandlerPackageSave.h"
#include "Elements/PCGStaticMeshSpawner.h"
#include "Engine/World.h"
#include "MeshSelectors/PCGMeshSelectorWeighted.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGVolume.h"
#include "UObject/Package.h"

namespace PCGHandlerTests
{
	const TCHAR* const Root = TEXT("/UEMCPPCGTest/");

	TSharedPtr<FJsonObject> Call(FMCPHandlerRegistry& Registry, const TCHAR* Method, const TSharedPtr<FJsonObject>& Params)
	{
		const TSharedPtr<FJsonValue> Value = Registry.ExecuteHandler(Method, Params);
		return Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : MakeShared<FJsonObject>();
	}

	bool Succeeded(const TSharedPtr<FJsonObject>& Result)
	{
		bool bSuccess = false;
		return Result->TryGetBoolField(TEXT("success"), bSuccess) && bSuccess;
	}

	/** A fresh editor world made current for the test, restored on the way out. */
	struct FWorldScope
	{
		UWorld* OriginalWorld = nullptr;
		UWorld* World = nullptr;

		FWorldScope()
		{
			if (!GEditor) return;
			OriginalWorld = GEditor->GetEditorWorldContext().World();
			const UWorld::InitializationValues Initialization = UWorld::InitializationValues()
				.InitializeScenes(false).AllowAudioPlayback(false).RequiresHitProxies(false)
				.CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false)
				.ShouldSimulatePhysics(false).EnableTraceCollision(false).SetTransactional(false)
				.CreateFXSystem(false).CreateWorldPartition(false);
			World = UWorld::CreateWorld(EWorldType::Editor, false,
				MakeUniqueObjectName(GetTransientPackage(), UWorld::StaticClass(), TEXT("UEMCP_PCGTest")),
				GetTransientPackage(), true, ERHIFeatureLevel::Num, &Initialization);
			if (World) GEditor->GetEditorWorldContext().SetCurrentWorld(World);
		}

		~FWorldScope()
		{
			if (!World) return;
			GEditor->GetEditorWorldContext().SetCurrentWorld(OriginalWorld);
			World->DestroyWorld(false);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPPCGSpawnerSelectorTest,
	"UE.MCP.PCG.Graph.PathFolderAndSpawnerSelector",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPPCGSpawnerSelectorTest::RunTest(const FString& Parameters)
{
	using namespace PCGHandlerTests;
	const FMCPScopedTestMount Mount{ FString(Root), TEXT("UEMCPPCGTest") };
	FMCPHandlerRegistry Registry;
	FPCGHandlers::RegisterHandlers(Registry);

	// create_pcg_graph: 'path' names the folder, as packagePath does.
	const FString Folder = FString(Root) + TEXT("Sub");
	auto CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("name"), TEXT("PCG_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	CreateParams->SetStringField(TEXT("path"), Folder);
	const TSharedPtr<FJsonObject> Created = Call(Registry, TEXT("create_pcg_graph"), CreateParams);
	if (!TestTrue(TEXT("create_pcg_graph succeeds"), Succeeded(Created))) return false;
	const FString GraphPath = Created->GetStringField(TEXT("path"));
	TestTrue(TEXT("the graph is created in the folder 'path' names"), GraphPath.StartsWith(Folder + TEXT("/")));

	auto AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("assetPath"), GraphPath);
	AddParams->SetStringField(TEXT("nodeType"), TEXT("PCGStaticMeshSpawnerSettings"));
	const TSharedPtr<FJsonObject> Added = Call(Registry, TEXT("add_pcg_node"), AddParams);
	if (!TestTrue(TEXT("add_pcg_node adds a spawner"), Succeeded(Added))) return false;
	const FString NodeName = Added->GetStringField(TEXT("nodeName"));

	UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *GraphPath);
	if (!TestNotNull(TEXT("the graph loads"), Graph)) return false;
	UPCGNode* Node = nullptr;
	for (UPCGNode* Candidate : Graph->GetNodes())
	{
		if (Candidate && Candidate->GetName() == NodeName) Node = Candidate;
	}
	UPCGStaticMeshSpawnerSettings* Spawner = Node ? Cast<UPCGStaticMeshSpawnerSettings>(const_cast<UPCGSettings*>(Node->GetSettings())) : nullptr;
	if (!TestNotNull(TEXT("the spawner settings are reachable"), Spawner)) return false;

	auto WriteSettings = [&](const TCHAR* Key, const TSharedPtr<FJsonValue>& Value)
	{
		auto Settings = MakeShared<FJsonObject>();
		Settings->SetField(Key, Value);
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("assetPath"), GraphPath);
		Params->SetStringField(TEXT("nodeName"), NodeName);
		Params->SetObjectField(TEXT("settings"), Settings);
		return Call(Registry, TEXT("set_pcg_node_settings"), Params);
	};

	// A type change goes through SetMeshSelectorType, so the selector follows it.
	const TSharedPtr<FJsonObject> ByAttribute = WriteSettings(TEXT("MeshSelectorType"), MakeShared<FJsonValueString>(TEXT("/Script/PCG.PCGMeshSelectorByAttribute")));
	TestTrue(TEXT("writing MeshSelectorType succeeds"), Succeeded(ByAttribute));
	TestTrue(TEXT("the selector is recreated as the new type"),
		Spawner->MeshSelectorParameters && Spawner->MeshSelectorParameters->GetClass()->GetName() == TEXT("PCGMeshSelectorByAttribute"));

	// Type already weighted, selector stale and owned elsewhere: rewriting the
	// unchanged type still recreates an owned selector.
	Spawner->MeshSelectorType = UPCGMeshSelectorWeighted::StaticClass();
	UPCGMeshSelectorWeighted* Foreign = NewObject<UPCGMeshSelectorWeighted>(GetTransientPackage());
	Spawner->MeshSelectorParameters = Foreign;
	const TSharedPtr<FJsonObject> Repaired = WriteSettings(TEXT("MeshSelectorType"), MakeShared<FJsonValueString>(TEXT("/Script/PCG.PCGMeshSelectorWeighted")));
	TestTrue(TEXT("rewriting the unchanged type succeeds"), Succeeded(Repaired));
	TestTrue(TEXT("the stale selector is replaced"), Spawner->MeshSelectorParameters && Spawner->MeshSelectorParameters != Foreign);
	TestTrue(TEXT("the new selector is weighted and owned by the settings"),
		Cast<UPCGMeshSelectorWeighted>(Spawner->MeshSelectorParameters) && Spawner->MeshSelectorParameters->GetOuter() == Spawner);
	TestTrue(TEXT("the replacement is reported"), Repaired->HasField(TEXT("recreatedInstancedParameters")));

	// A path for the instanced selector is refused with nothing changed.
	UObject* OwnSelector = Spawner->MeshSelectorParameters;
	const TSharedPtr<FJsonObject> PathWrite = WriteSettings(TEXT("MeshSelectorParameters"), MakeShared<FJsonValueString>(Foreign->GetPathName()));
	TestFalse(TEXT("a path for the selector is refused"), Succeeded(PathWrite));
	TestEqual(TEXT("the refusal names its reason"), PathWrite->GetStringField(TEXT("reason")), FString(TEXT("instanced_reference_path")));
	TestTrue(TEXT("the selector is untouched"), Spawner->MeshSelectorParameters == OwnSelector);

	// set_static_mesh_spawner_meshes writes into the owned selector.
	auto Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("mesh"), TEXT("/Engine/BasicShapes/Cube.Cube"));
	auto MeshParams = MakeShared<FJsonObject>();
	MeshParams->SetStringField(TEXT("assetPath"), GraphPath);
	MeshParams->SetStringField(TEXT("nodeName"), NodeName);
	TArray<TSharedPtr<FJsonValue>> Entries;
	Entries.Add(MakeShared<FJsonValueObject>(Entry));
	MeshParams->SetArrayField(TEXT("entries"), Entries);
	const TSharedPtr<FJsonObject> Meshes = Call(Registry, TEXT("set_static_mesh_spawner_meshes"), MeshParams);
	TestTrue(TEXT("set_static_mesh_spawner_meshes succeeds and saves"), Succeeded(Meshes));
	bool bOwned = false;
	TestTrue(TEXT("it reports the selector is the settings' own"), Meshes->TryGetBoolField(TEXT("selectorOuterIsSettings"), bOwned) && bOwned);

	// Export leaves the instanced selector out; import refuses a path for it.
	auto ExportParams = MakeShared<FJsonObject>();
	ExportParams->SetStringField(TEXT("assetPath"), GraphPath);
	const TSharedPtr<FJsonObject> Exported = Call(Registry, TEXT("export_pcg_graph"), ExportParams);
	bool bOmitted = false;
	for (const TSharedPtr<FJsonValue>& NodeValue : Exported->GetArrayField(TEXT("nodes")))
	{
		const TSharedPtr<FJsonObject> NodeObj = NodeValue->AsObject();
		if (NodeObj->GetStringField(TEXT("name")) != NodeName) continue;
		TestFalse(TEXT("export omits the selector object"), NodeObj->GetObjectField(TEXT("settings"))->HasField(TEXT("MeshSelectorParameters")));
		const TArray<TSharedPtr<FJsonValue>>* Omitted = nullptr;
		bOmitted = NodeObj->TryGetArrayField(TEXT("omittedInstancedProperties"), Omitted) && Omitted
			&& Omitted->ContainsByPredicate([](const TSharedPtr<FJsonValue>& V) { return V->AsString() == TEXT("MeshSelectorParameters"); });
	}
	TestTrue(TEXT("export lists the omitted selector"), bOmitted);

	auto ImportSettings = MakeShared<FJsonObject>();
	ImportSettings->SetStringField(TEXT("MeshSelectorParameters"), OwnSelector->GetPathName());
	auto ImportNode = MakeShared<FJsonObject>();
	ImportNode->SetStringField(TEXT("name"), TEXT("Copy"));
	ImportNode->SetStringField(TEXT("class"), TEXT("PCGStaticMeshSpawnerSettings"));
	ImportNode->SetObjectField(TEXT("settings"), ImportSettings);
	auto ImportParams = MakeShared<FJsonObject>();
	ImportParams->SetStringField(TEXT("assetPath"), GraphPath);
	TArray<TSharedPtr<FJsonValue>> ImportNodes;
	ImportNodes.Add(MakeShared<FJsonValueObject>(ImportNode));
	ImportParams->SetArrayField(TEXT("nodes"), ImportNodes);
	const int32 NodesBefore = Graph->GetNodes().Num();
	const TSharedPtr<FJsonObject> Imported = Call(Registry, TEXT("import_pcg_graph"), ImportParams);
	TestFalse(TEXT("import refuses a path for the selector"), Succeeded(Imported));
	TestEqual(TEXT("and adds no node"), Graph->GetNodes().Num(), NodesBefore);

	// A refused mesh path must not replace a different selector or dirty the package.
	WriteSettings(TEXT("MeshSelectorType"), MakeShared<FJsonValueString>(TEXT("/Script/PCG.PCGMeshSelectorByAttribute")));
	UObject* BeforeBadMesh = Spawner->MeshSelectorParameters;
	Graph->GetOutermost()->SetDirtyFlag(false);
	AddExpectedError(TEXT("LoadAsset failed"), EAutomationExpectedErrorFlags::Contains, 0);
	Entry->SetStringField(TEXT("mesh"), TEXT("/Engine/BasicShapes/MissingMesh.MissingMesh"));
	TestFalse(TEXT("invalid mesh is refused"), Succeeded(Call(Registry, TEXT("set_static_mesh_spawner_meshes"), MeshParams)));
	TestTrue(TEXT("invalid mesh preserves selector and type"), Spawner->MeshSelectorParameters == BeforeBadMesh
		&& Spawner->MeshSelectorType == BeforeBadMesh->GetClass());
	TestFalse(TEXT("invalid mesh does not dirty the graph"), Graph->GetOutermost()->IsDirty());

	// Type notifications must finish before nested writes, independent of JSON order.
	auto WeightedEntry = MakeShared<FJsonObject>();
	WeightedEntry->SetNumberField(TEXT("Weight"), 7);
	TArray<TSharedPtr<FJsonValue>> WeightedEntries{MakeShared<FJsonValueObject>(WeightedEntry)};
	for (bool bTypeFirst : {true, false})
	{
		WriteSettings(TEXT("MeshSelectorType"), MakeShared<FJsonValueString>(TEXT("/Script/PCG.PCGMeshSelectorByAttribute")));
		auto Batch = MakeShared<FJsonObject>();
		if (bTypeFirst) Batch->SetStringField(TEXT("MeshSelectorType"), TEXT("/Script/PCG.PCGMeshSelectorWeighted"));
		Batch->SetArrayField(TEXT("MeshSelectorParameters.MeshEntries"), WeightedEntries);
		if (!bTypeFirst) Batch->SetStringField(TEXT("MeshSelectorType"), TEXT("/Script/PCG.PCGMeshSelectorWeighted"));
		auto BatchParams = MakeShared<FJsonObject>();
		BatchParams->SetStringField(TEXT("assetPath"), GraphPath);
		BatchParams->SetStringField(TEXT("nodeName"), NodeName);
		BatchParams->SetObjectField(TEXT("settings"), Batch);
		for (int32 Repeat = 0; Repeat < 2; ++Repeat)
		{
			const auto BatchResult = Call(Registry, TEXT("set_pcg_node_settings"), BatchParams);
			TestTrue(TEXT("type plus entries succeeds, including same-type rewrite"), Succeeded(BatchResult));
			TestFalse(TEXT("rollback excludes entries owned by a replaced selector"), BatchResult->GetObjectField(TEXT("previousProperties"))->HasField(TEXT("MeshSelectorParameters.MeshEntries")));
			TestTrue(TEXT("the selector-content loss is disclosed"), BatchResult->GetBoolField(TEXT("rollbackLossy")));
			const auto* Weighted = Cast<UPCGMeshSelectorWeighted>(Spawner->MeshSelectorParameters);
			TestTrue(TEXT("entries survive all settings notifications"), Weighted && Weighted->MeshEntries.Num() == 1 && Weighted->MeshEntries[0].Weight == 7);
			if (Repeat == 0)
			{
				const auto Inverse = BatchResult->GetObjectField(TEXT("rollback"))->GetObjectField(TEXT("payload"));
				TestTrue(TEXT("type-changing batch rollback succeeds"), Succeeded(Call(Registry, TEXT("set_pcg_node_settings"), Inverse)));
				TestEqual(TEXT("rollback restores the original type"), Spawner->MeshSelectorParameters->GetClass()->GetName(), FString(TEXT("PCGMeshSelectorByAttribute")));
				Call(Registry, TEXT("set_pcg_node_settings"), BatchParams);
			}
		}

		ImportNode->SetObjectField(TEXT("settings"), Batch);
		const auto BatchImported = Call(Registry, TEXT("import_pcg_graph"), ImportParams);
		TestTrue(TEXT("import accepts type plus entries in either order"), Succeeded(BatchImported));
		const auto* ImportedSpawner = Cast<UPCGStaticMeshSpawnerSettings>(Graph->GetNodes().Last()->GetSettings());
		const auto* ImportedWeighted = ImportedSpawner ? Cast<UPCGMeshSelectorWeighted>(ImportedSpawner->MeshSelectorParameters) : nullptr;
		TestTrue(TEXT("imported entries survive notifications"), ImportedWeighted && ImportedWeighted->MeshEntries.Num() == 1 && ImportedWeighted->MeshEntries[0].Weight == 7);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPPCGImportWarningsTest,
	"UE.MCP.PCG.Graph.ImportWarningsAndFailures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPPCGImportWarningsTest::RunTest(const FString& Parameters)
{
	using namespace PCGHandlerTests;
	const FMCPScopedTestMount Mount{ FString(Root), TEXT("UEMCPPCGTest") };
	FMCPHandlerRegistry Registry;
	FPCGHandlers::RegisterHandlers(Registry);
	const FString PackageName = FString(Root) + TEXT("PG_Import_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* Package = CreatePackage(*PackageName);
	UPCGGraph* Graph = NewObject<UPCGGraph>(Package, TEXT("PG_Import"), RF_Public | RF_Standalone | RF_Transactional);
	const FString ChildPackageName = PackageName + TEXT("_Child");
	UPCGGraph* Child = NewObject<UPCGGraph>(CreatePackage(*ChildPackageName), TEXT("Child"), RF_Public | RF_Standalone | RF_Transactional);
	Child->AddUserParameters({FPropertyBagPropertyDesc(TEXT("Preserved"), EPropertyBagPropertyType::Bool)});
	FString SaveError;
	if (!TestTrue(TEXT("child graph saves"), SaveAssetPackageChecked(Child, SaveError))) return false;
	UPCGSubgraphSettings* FromSettings = nullptr;
	UPCGAddTagSettings* ToSettings = nullptr;
	UPCGNode* From = Graph->AddNodeOfType(FromSettings);
	UPCGNode* To = Graph->AddNodeOfType(ToSettings);
	if (!TestTrue(TEXT("two nodes added"), From && To)) return false;
	FromSettings->SetSubgraph(Child);
	From->UpdateAfterSettingsChangeDuringCreation();
	auto OverrideParams = MakeShared<FJsonObject>();
	OverrideParams->SetStringField(TEXT("assetPath"), Graph->GetPathName());
	OverrideParams->SetStringField(TEXT("nodeName"), From->GetName());
	auto OverrideValues = MakeShared<FJsonObject>();
	OverrideValues->SetBoolField(TEXT("Preserved"), true);
	OverrideParams->SetObjectField(TEXT("parameters"), OverrideValues);
	if (!TestTrue(TEXT("subgraph override is set"), Succeeded(Call(Registry, TEXT("set_pcg_subgraph_parameters"), OverrideParams)))) return false;
	auto CheckSubgraph = [&]()
	{
		const UPCGSubgraphSettings* Restored = nullptr;
		for (const UPCGNode* Candidate : Graph->GetNodes())
		{
			if (const auto* Sub = Cast<UPCGSubgraphSettings>(Candidate->GetSettings())) Restored = Sub;
		}
		if (!TestTrue(TEXT("subgraph asset and owned instance survive"), Restored && Restored->SubgraphInstance
			&& Restored->SubgraphInstance->Graph == Child && Restored->SubgraphInstance->GetOuter() == Restored)) return;
		const auto* Bag = Restored->SubgraphInstance->GetUserParametersStruct();
		const auto Value = Bag->GetValueBool(TEXT("Preserved"));
		TestTrue(TEXT("subgraph parameter value survives"), Value.HasValue() && Value.GetValue());
		const auto* Desc = Bag->GetPropertyBagStruct()->FindPropertyDescByName(TEXT("Preserved"));
		TestTrue(TEXT("subgraph override flag survives"), Desc && Restored->SubgraphInstance->IsPropertyOverridden(Desc->CachedProperty));
	};
	Graph->AddEdge(From, PCGPinConstants::DefaultOutputLabel, To, PCGPinConstants::DefaultInputLabel);
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("assetPath"), Graph->GetPathName());
	const auto Exported = Call(Registry, TEXT("export_pcg_graph"), Params);
	Params->SetBoolField(TEXT("replace"), true);
	Params->SetArrayField(TEXT("nodes"), Exported->GetArrayField(TEXT("nodes")));
	Params->SetArrayField(TEXT("connections"), Exported->GetArrayField(TEXT("connections")));
	const auto Imported = Call(Registry, TEXT("import_pcg_graph"), Params);
	TestTrue(TEXT("replace round trip succeeds before removed names are collected"), Succeeded(Imported));
	// The engine may free a removed node's name before the re-add, so a collision
	// warning is possible but not guaranteed. Any warning must be only that.
	const TArray<TSharedPtr<FJsonValue>>* ImportWarnings = nullptr;
	if (Imported->TryGetArrayField(TEXT("warnings"), ImportWarnings))
	{
		for (const TSharedPtr<FJsonValue>& Warning : *ImportWarnings)
		{
			TestTrue(TEXT("only name collisions are warned about"), Warning->AsString().Contains(TEXT("name collision")));
		}
	}
	TestEqual(TEXT("both nodes landed"), Imported->GetNumberField(TEXT("nodesCreated")), 2.0);
	TestEqual(TEXT("the edge landed"), Imported->GetNumberField(TEXT("connectionsMade")), 1.0);
	CheckSubgraph();
	const auto Rollback = Imported->GetObjectField(TEXT("rollback"))->GetObjectField(TEXT("payload"));
	const auto RolledBack = Call(Registry, TEXT("import_pcg_graph"), Rollback);
	TestTrue(TEXT("the import's rollback also succeeds with name collisions"), Succeeded(RolledBack));
	TestEqual(TEXT("rollback restores the subgraph edge"), RolledBack->GetNumberField(TEXT("connectionsMade")), 1.0);
	CheckSubgraph();

	// Actual losses still fail, whether a node, a setting, or a connection.
	auto Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("name"), TEXT("FailureProbe"));
	Node->SetStringField(TEXT("class"), TEXT("ClassThatDoesNotExist"));
	Params->SetArrayField(TEXT("nodes"), {MakeShared<FJsonValueObject>(Node)});
	Params->RemoveField(TEXT("connections"));
	TestFalse(TEXT("missing node class fails"), Succeeded(Call(Registry, TEXT("import_pcg_graph"), Params)));
	Node->SetStringField(TEXT("class"), TEXT("PCGAddTagSettings"));
	auto BadSettings = MakeShared<FJsonObject>();
	BadSettings->SetBoolField(TEXT("PropertyThatDoesNotExist"), true);
	Node->SetObjectField(TEXT("settings"), BadSettings);
	TestFalse(TEXT("unapplied setting fails"), Succeeded(Call(Registry, TEXT("import_pcg_graph"), Params)));
	Node->RemoveField(TEXT("settings"));
	auto Edge = MakeShared<FJsonObject>();
	Edge->SetStringField(TEXT("from"), TEXT("FailureProbe"));
	Edge->SetStringField(TEXT("to"), TEXT("MissingTarget"));
	Params->SetArrayField(TEXT("connections"), {MakeShared<FJsonValueObject>(Edge)});
	TestFalse(TEXT("unapplied edge fails"), Succeeded(Call(Registry, TEXT("import_pcg_graph"), Params)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPPCGVolumeAndRunTest,
	"UE.MCP.PCG.Volume.TransformAndRunRecord",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPPCGVolumeAndRunTest::RunTest(const FString& Parameters)
{
	using namespace PCGHandlerTests;
	FWorldScope Scope;
	if (!TestNotNull(TEXT("disposable editor world exists"), Scope.World)) return false;
	FMCPHandlerRegistry Registry;
	FPCGHandlers::RegisterHandlers(Registry);

	const FTransform Requested(FRotator(10.0, 20.0, 30.0), FVector(100.0, -200.0, 300.0), FVector(2.0, 3.0, 0.5));
	auto Transform = MakeShared<FJsonObject>();
	Transform->SetObjectField(TEXT("location"), MCPVec3ToJsonObject(Requested.GetLocation()));
	Transform->SetObjectField(TEXT("rotation"), MCPRotatorToJsonObject(Requested.Rotator()));
	Transform->SetObjectField(TEXT("scale3D"), MCPVec3ToJsonObject(Requested.GetScale3D()));
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("label"), TEXT("UEMCP_PCGVolume"));
	Params->SetObjectField(TEXT("transform"), Transform);
	Params->SetObjectField(TEXT("extent"), MCPVec3ToJsonObject(FVector(50.0)));
	const TSharedPtr<FJsonObject> Placed = Call(Registry, TEXT("add_pcg_volume"), Params);
	if (!TestTrue(TEXT("add_pcg_volume succeeds"), Succeeded(Placed))) return false;

	TSharedPtr<FJsonValue> LookupError;
	AActor* Volume = MCPResolveActorToken(Scope.World, TEXT("UEMCP_PCGVolume"), LookupError);
	if (!TestNotNull(TEXT("the volume is in the disposable world"), Volume)) return false;
	TestTrue(TEXT("location is honoured"), Volume->GetActorLocation().Equals(Requested.GetLocation(), 0.01));
	TestTrue(TEXT("rotation is honoured"), Volume->GetActorQuat().Equals(Requested.GetRotation(), 0.0001));
	TestTrue(TEXT("scale3D is honoured"), Volume->GetActorScale3D().Equals(Requested.GetScale3D(), 0.0001));
	FVector ScaledExtent;
	TestTrue(TEXT("the scaled extent is reported"), ReadVec3Fields(Placed->GetObjectField(TEXT("scaledExtent")), ScaledExtent)
		&& ScaledExtent.Equals(FVector(100.0, 150.0, 25.0), 0.01));

	// A volume with no graph: Generate schedules nothing, so the run settles at once.
	auto RunParams = MakeShared<FJsonObject>();
	RunParams->SetStringField(TEXT("actorLabel"), TEXT("UEMCP_PCGVolume"));
	const TSharedPtr<FJsonObject> Run = Call(Registry, TEXT("execute_pcg_graph"), RunParams);
	if (!TestTrue(TEXT("execute_pcg_graph succeeds"), Succeeded(Run))) return false;
	const double RunId = Run->GetNumberField(TEXT("runId"));
	TestTrue(TEXT("a run id is returned"), RunId > 0);
	TestEqual(TEXT("nothing to generate settles as not_started"), Run->GetObjectField(TEXT("lastRun"))->GetStringField(TEXT("state")), FString(TEXT("not_started")));

	const TSharedPtr<FJsonObject> Details = Call(Registry, TEXT("get_pcg_component_details"), RunParams);
	if (!TestTrue(TEXT("get_pcg_component_details succeeds"), Succeeded(Details))) return false;
	const TSharedPtr<FJsonObject> Comp = Details->GetArrayField(TEXT("components"))[0]->AsObject();
	TestFalse(TEXT("the component is not generating"), Comp->GetBoolField(TEXT("isGenerating")));
	TestTrue(TEXT("completion fields are present"), Comp->HasField(TEXT("isCleaningUp")) && Comp->HasField(TEXT("generated")) && Comp->HasField(TEXT("partitioned")));
	TestEqual(TEXT("details carry the same run"), Comp->GetObjectField(TEXT("lastRun"))->GetNumberField(TEXT("id")), RunId);

	// Schedule work without ticking the world: normal execute refuses a second run,
	// while force_regenerate remains a recovery path and supplies a fresh run id.
	UPCGComponent* PCGComponent = CastChecked<APCGVolume>(Volume)->PCGComponent;
	PCGComponent->SetGraph(NewObject<UPCGGraph>(GetTransientPackage()));
	const auto Pending = Call(Registry, TEXT("execute_pcg_graph"), RunParams);
	if (!TestTrue(TEXT("generation is pending before the world ticks"), PCGComponent->IsGenerating())) return false;
	TestFalse(TEXT("ordinary execute refuses while generating"), Succeeded(Call(Registry, TEXT("execute_pcg_graph"), RunParams)));
	const auto Forced = Call(Registry, TEXT("force_regenerate_pcg"), RunParams);
	TestTrue(TEXT("force regeneration accepts a busy component"), Succeeded(Forced));
	TestTrue(TEXT("force regeneration starts a newer run"), Forced->GetNumberField(TEXT("runId")) > Pending->GetNumberField(TEXT("runId")));
	PCGComponent->CancelGeneration();

	return true;
}

#endif
