#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"

class UBehaviorTree;
class UBlackboardData;
class UBTNode;

class FGameplayHandlers
{
public:
	static void RegisterHandlers(class FMCPHandlerRegistry& Registry);

	// BehaviorTree introspection primitives, shared by the BT handlers and
	// exercised directly by the automation tests in Private/Tests, which build
	// nodes in memory rather than round-tripping an asset through disk.
	// Definitions live in GameplayHandlers_BehaviorTree.cpp.

	/** Every key on a Blackboard asset: name, type, sync flag, description. */
	static TArray<TSharedPtr<FJsonValue>> DescribeBlackboardKeys(const UBlackboardData* Blackboard);

	/** One BT node as JSON: identity, decorator configuration (#888), and the
	 *  node's own UPROPERTY values when asked for (#919). PropertyFilter holds
	 *  lowercased property names; empty means "everything the node class adds". */
	static TSharedPtr<FJsonObject> DescribeBTNode(
		UBTNode* Node,
		const FString& NodePath,
		const FString& ParentPath,
		bool bIncludeProperties,
		bool bIncludeInherited,
		const TSet<FString>& PropertyFilter);

	/** Read one dotted/indexed property path off a BT node. A UE 5.8
	 *  FValueOrBBKey_* field comes back unpacked into defaultValue + key. */
	static TSharedPtr<FJsonValue> ReadBTNodeProperty(UBTNode* Node, const FString& PropertyPath, FString& OutError);

	/** Write one dotted/indexed property path on a BT node. A scalar aimed at a
	 *  FValueOrBBKey_* field lands on its DefaultValue; a class-valued leaf
	 *  accepts a short name, a /Script path or a Blueprint asset path. */
	static bool WriteBTNodeProperty(UBTNode* Node, const FString& PropertyPath, const TSharedPtr<FJsonValue>& Value, FString& OutError);

	/** Load a BehaviorTree from a caller-supplied asset path, or null. */
	static UBehaviorTree* LoadBehaviorTree(const FString& AssetPath);

	/** Every runtime node in a compiled tree, keyed by the structural address
	 *  the read surface reports ("Root.Children[0].Decorators[1]").
	 *
	 *  #889 authoring addresses editor-graph nodes by guid, but a caller who
	 *  just read the tree holds these addresses instead, so both surfaces walk
	 *  this one map rather than growing a second scheme. Defined next to the
	 *  walker it wraps in GameplayHandlers_BehaviorTree.cpp. */
	static void MapBTNodeAddresses(UBehaviorTree* Tree, TMap<UBTNode*, FString>& OutAddresses);

private:
	static TSharedPtr<FJsonValue> CreateSmartObjectDefinition(const TSharedPtr<FJsonObject>& Params);
	// #778: per-player Enhanced Input applied mapping contexts, per PIE world.
	static TSharedPtr<FJsonValue> GetInputMappingContexts(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> GetNavmeshInfo(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> GetGameFrameworkInfo(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> ListInputAssets(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> ListBehaviorTrees(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> ListEqsQueries(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> ListStateTrees(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> ProjectPointToNavigation(const TSharedPtr<FJsonObject>& Params);
	// Enhanced Input asset authoring lives here (core authoring, not test
	// automation). pie-studio handles PIE record/replay/inject of inputs.
	static TSharedPtr<FJsonValue> CreateInputAction(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> CreateInputMappingContext(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> ReadImc(const TSharedPtr<FJsonObject>& Params);
	// #604 read a live PIE player's applied Input Mapping Contexts
	static TSharedPtr<FJsonValue> AddImcMapping(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> SetMappingModifiers(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> RemoveImcMapping(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> SetImcMappingKey(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> SetImcMappingAction(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> CreateBlackboard(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> CreateBehaviorTree(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> CreateEqsQuery(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> CreateStateTree(const TSharedPtr<FJsonObject>& Params);
	// #654: read a running StateTreeComponent's active state names in PIE.
	static TSharedPtr<FJsonValue> GetStateTreeRuntime(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> CreateGameMode(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> CreateGameState(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> CreatePlayerController(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> CreatePlayerState(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> CreateHud(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> SpawnNavModifierVolume(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> RebuildNavmesh(const TSharedPtr<FJsonObject>& Params);
	// #424: synchronous path query + invoker enumeration.
	static TSharedPtr<FJsonValue> FindNavPath(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> ListNavInvokers(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> SetWorldGameMode(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> AddBlackboardKey(const TSharedPtr<FJsonObject>& Params);
	// #469: child-of-parent blackboard pattern + per-key removal + read.
	static TSharedPtr<FJsonValue> SetBlackboardParent(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> RemoveBlackboardKey(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> ReadBlackboard(const TSharedPtr<FJsonObject>& Params);
	// #494: discover available BT node classes so authoring scripts can
	// build asset-specific BTs without grepping the engine source.
	static TSharedPtr<FJsonValue> ListBTNodeClasses(const TSharedPtr<FJsonObject>& Params);
	// #250: rebind a BehaviorTree asset's BlackboardAsset (the C++ field is
	// protected, so reflection is the only way to write it cleanly).
	static TSharedPtr<FJsonValue> SetBehaviorTreeBlackboard(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> GetBehaviorTreeInfo(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> AddPerceptionComponent(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> ConfigureAiPerceptionSense(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> AddStateTreeComponent(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> AddSmartObjectComponent(const TSharedPtr<FJsonObject>& Params);
	// #416: slot authoring on USmartObjectDefinition via UPROPERTY reflection
	// (no Build.cs dependency on SmartObjectsModule).
	static TSharedPtr<FJsonValue> AddSmartObjectSlot(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> SetSmartObjectSlot(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> RemoveSmartObjectSlot(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> ListSmartObjectSlots(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> AddSmartObjectSlotBehavior(const TSharedPtr<FJsonObject>& Params);

	// IMC read/write, PIE inspection, anim state, subsystem state - moved to pie-studio

	// Helper to create a blueprint with a given parent class
	static TSharedPtr<FJsonValue> CreateBlueprintWithParent(const FString& Name, const FString& PackagePath, const FString& ParentClassPath, const FString& FriendlyTypeName);

	// v0.7.11 - BT graph traversal (#124), decorator configuration added (#888)
	static TSharedPtr<FJsonValue> ReadBehaviorTreeGraph(const TSharedPtr<FJsonObject>& Params);

	// #919: pick BT nodes by class/name/path/kind and return their UPROPERTY
	// values, instead of an unfiltered whole-asset property dump.
	static TSharedPtr<FJsonValue> ReadBTNodeProperties(const TSharedPtr<FJsonObject>& Params);

	// #940: inventory BTTask nodes across one asset or a directory, including
	// the UE 5.8 FValueOrBBKey_Class shape of BTTask_MoveTo::FilterClass.
	static TSharedPtr<FJsonValue> ListBTTasks(const TSharedPtr<FJsonObject>& Params);

	// #919/#940: scoped nested write onto one owned BT node subobject.
	// Registered as both set_bt_node_property and set_bt_task_property.
	static TSharedPtr<FJsonValue> SetBTNodeProperty(const TSharedPtr<FJsonObject>& Params);

	// #889/#947: BehaviorTree editor-graph authoring. These drive
	// UBehaviorTreeGraph and its node classes, then recompile the graph into
	// the runnable UBTCompositeNode tree, which is the step no reflected
	// runtime API can perform. Definitions in GameplayHandlers_BTAuthoring.cpp.
	static TSharedPtr<FJsonValue> ListBTGraphNodes(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> AddBTNode(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> MoveBTNode(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> RemoveBTNode(const TSharedPtr<FJsonObject>& Params);

	// #163 - detailed navmesh configuration
	static TSharedPtr<FJsonValue> GetNavmeshDetails(const TSharedPtr<FJsonObject>& Params);

	// ApplyDamageInPie moved to pie-studio

};
