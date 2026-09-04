using UnrealBuildTool;

public class UE_MCP_Bridge : ModuleRules
{
	// Touched when Private/EngineStatus.cpp was added, and again for
	// Private/Handlers/WidgetHandlers_Extraction.cpp plus the first file under
	// Private/Tests: UBT caches the module's file list and will not pick up a
	// new .cpp until this file changes.
	// Private/Handlers/AssetHandlers_BulkUpsert.cpp: UBT caches the module's
	// file list and will not pick up a new .cpp until this file changes.
	// Private/Handlers/LevelHandlers_Inspection.cpp and its focused test: same reason.
	// Private/BridgeStateFiles.cpp, Private/BridgeParamEcho.cpp and
	// Private/Tests/BridgeProtocolTests.cpp: same reason.
	// Private/Tests/SequencerHandlerTests.cpp: same reason.
	// Private/Handlers/LevelHandlers_InstanceProjection.cpp and
	// Private/Tests/LevelInstanceProjectionTests.cpp: same reason.
	// Private/Tests/PackageSaveExtensionTests.cpp: same reason.
	// Private/Handlers/AnimationHandlers_ControlRigSequencer.cpp,
	// Private/Handlers/AnimationHandlers_Validation.cpp,
	// Private/Handlers/AnimationHandlers_IKRigAuthoring.cpp and
	// Private/Handlers/AnimationHandlers_IKRetargeterAuthoring.cpp plus
	// Private/Tests/AnimationControlRigTimelineTests.cpp: same reason.
	// Private/Tests/DataTableRowWriteTests.cpp: same reason.
	// Private/Handlers/EditorHandlers_RuntimeVisibility.cpp and
	// Private/Tests/RuntimeVisibilityTests.cpp: same reason.
	// Private/Handlers/MaterialHandlers_Build.cpp and
	// Private/Tests/MaterialBuildTests.cpp: same reason.
	// Private/Handlers/AssetHandlers_Geometry.cpp,
	// Private/Handlers/AnimationHandlers_Pose.cpp,
	// Private/Tests/MeshGeometryTests.cpp and
	// Private/Tests/AnimationPoseTests.cpp: same reason.
	// Private/Tests/WidgetBlueprintResolveTests.cpp and
	// Private/Tests/GasLiveAttributeTests.cpp: same reason.
	// Private/Tests/BlueprintHandlerSurfaceTests.cpp: same reason.
	// Private/Handlers/BlueprintHandlers_Search.cpp: same reason.
	// Private/Handlers/LevelHandlers_Query.cpp,
	// Private/Handlers/LevelHandlers_BatchWrite.cpp,
	// Private/Handlers/LevelHandlers_Convert.cpp,
	// Private/Handlers/LevelHandlers_Refresh.cpp,
	// Private/Handlers/LevelHandlers_Transient.cpp,
	// Private/Handlers/AssetHandlers_BulkRead.cpp,
	// Private/Handlers/FoliageHandlers_Batch.cpp,
	// Private/Tests/LevelQueryComponentsTests.cpp,
	// Private/Tests/LevelBatchWriteTests.cpp,
	// Private/Tests/LevelRefreshTests.cpp,
	// Private/Tests/LevelConvertBrushesTests.cpp,
	// Private/Tests/LevelTransientActorTests.cpp,
	// Private/Tests/AssetBulkReadTests.cpp and
	// Private/Tests/FoliageBatchTests.cpp: same reason.
	// Private/Tests/EditorFunctionCallTests.cpp (#885/#969): same reason.
	// Private/Tests/SequencerScrubTests.cpp (#881): same reason.
	// Private/Tests/AssetPathResolutionTests.cpp and
	// Private/Tests/AssetPropertyPersistTests.cpp: same reason.
	// Private/Handlers/AssetHandlers_Subobject.cpp,
	// Private/Handlers/AssetHandlers_Redirectors.cpp and
	// Private/Tests/AssetSubobjectTests.cpp: same reason.
	// Private/Tests/AssetDeleteGuardTests.cpp (#976): same reason.
	// Private/Tests/PackageWriteGuardTests.cpp (#932): same reason.
	// Private/Handlers/BlueprintHandlers_Collision.cpp and
	// Private/Tests/CollisionQueryTests.cpp (#925): same reason.
	// Private/Handlers/AssetHandlers_MeshBoolean.cpp and
	// Private/Tests/MeshBooleanTests.cpp (#916): same reason.
	// Private/Tests/LevelTraceWorldScopeTests.cpp (#933): same reason.
	// Private/Tests/LandscapeSampleTests.cpp (#939): same reason.
	// Private/Handlers/LevelHandlers_PostProcess.cpp and
	// Private/Tests/LevelPostProcessOverrideTests.cpp (#950): same reason.
	// Private/Handlers/LevelHandlers_Save.cpp and
	// Private/Tests/LevelSaveReportTests.cpp (#964): same reason.
	// Private/Tests/LevelComponentTreeInstanceCountTests.cpp (#986): same reason.
	// Private/Handlers/LevelHandlers_WorldPartitionSettings.cpp and
	// Private/Tests/LevelWorldPartitionSettingsTests.cpp (#985): same reason.
	public UE_MCP_Bridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Json",
				"JsonUtilities",
				"GameplayTags",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AIModule",
				"ApplicationCore",
				// #889: authoring a BehaviorTree means driving UBehaviorTreeGraph
				// and its node classes, then calling UpdateAsset to compile the
				// graph into the runnable UBTCompositeNode tree. That orchestration
				// lives only in BehaviorTreeEditor (with FGraphNodeClassData and
				// UAIGraphNode::AddSubNode coming from AIGraph beneath it) and is
				// not exposed to script, so no reflected runtime API can stand in
				// for it. Both are editor modules, which this plugin already is:
				// its .uplugin declares Type "Editor" and UnrealEd, UMGEditor and
				// MaterialEditor are already linked the same way.
				"AIGraph",
				"BehaviorTreeEditor",
				"MessageLog",
				"AnimationCore",
				"AnimGraph",
				"AnimationEditor",
				// UAnimPoseExtensions / FAnimPoseEvaluationOptions (AnimPose.h),
				// the engine's own pose evaluator, used by animation(sample_pose)
				// and animation(measure_natural_speed).
				"AnimationBlueprintLibrary",
				"AnimationModifiers",
				"AssetRegistry",
				"AssetTools",
				"AudioEditor",
				"AudioMixer",
				"AudioExtensions",
				"MetasoundEngine",
				"MetasoundFrontend",
				"MetasoundGraphCore",
				"Synthesis",
				"BSPUtils",
				"BlueprintEditorLibrary",
				"BlueprintGraph",
				"Blutility",
				"Chooser",
				"ContentBrowser",
				"ControlRig",
				"ControlRigDeveloper",
				"ControlRigEditor",
				"RigVMDeveloper",
				"DataValidation",
				"EditorScriptingUtilities",
				"EditorStyle",
				"EditorSubsystem",
				"EditorWidgets",
				"EnhancedInput",
				"Foliage",
				"GameProjectGeneration",
				"GameplayAbilities",
				"GameplayTasks",
				"HTTP",
				"IKRig",
				"IKRigDeveloper",
				"IKRigEditor",
				"ImageWrapper",
				"InputCore",
				"Kismet",
				"KismetCompiler",
				"Landscape",
				"LevelEditor",
				"LevelSequence",
				"LevelSequenceEditor",
				"MainFrame",
				"MaterialEditor",
				"MovieScene",
				"MovieSceneTracks",
				"MeshDescription",
				"NavigationSystem",
				"Niagara",
				"NiagaraEditor",
				"PCG",
				"PCGEditor",
				"PoseSearch",
				"PoseSearchEditor",
				"PropertyBindingUtils",
				"PropertyEditor",
				"PythonScriptPlugin",
				"Sequencer",
				"Settings",
				"SkeletalMeshEditor",
				"Slate",
				"SlateCore",
				"StateTreeModule",
				"StateTreeEditorModule",
				"StaticMeshDescription",
				"ClothingSystemRuntimeCommon",
				"ClothingSystemRuntimeInterface",
				"SubobjectDataInterface",
				"ToolMenus",
				// The engine-status snapshot, in its own module so it can load
				// at PostConfigInit and cover the startup window that exists
				// before this module does.
				"UE_MCP_BridgeStatus",
				"RenderCore",
				"RHI",
				"UMG",
				"UMGEditor",
				"UnrealEd",
				"WebSockets",
				"WorkspaceMenuStructure",
			}
		);

		// LiveCoding is Windows-only (Developer/Windows/LiveCoding)
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}

		// Fab is Epic's marketplace plugin. It ships enabled by default on UE 5.8
		// but is absent on older engines and can be disabled, so we do not hard
		// depend on it: detect the plugin on disk and only then link its native
		// import/cache API, guarding those code paths with WITH_FAB_PLUGIN. When
		// absent, the Fab handlers still register and fall back to console-command
		// paths (login/sync/clear) or return a clean "not available" error.
		bool bFabPluginPresent = System.IO.Directory.Exists(
			System.IO.Path.Combine(EngineDirectory, "Plugins", "Fab"));
		if (bFabPluginPresent && Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("Fab");
			PublicDefinitions.Add("WITH_FAB_PLUGIN=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_FAB_PLUGIN=0");
		}
	}
}

// Rescan trigger: level-script handler automation coverage added.
// Rescan trigger: round 2 added handler translation units.
// Private/Toolbar/MCPToolbarStatus.cpp: toolbar connection-status indicator;
// same reason - UBT caches the module's file list.
// Rescan trigger: Private/Tests/GameThreadGateTests.cpp (#968).
// Rescan trigger: Private/Tests/AutomationRunnerTests.cpp (#993).
// Rescan trigger: Mass and skeletal-mesh handler translation units.
// Rescan trigger: FInstancedStruct scalar, wrapper-array and nested path tests added.
// Round 3: Private/Handlers/GameplayHandlers_BehaviorTree.cpp and
// Private/Tests/BehaviorTreeNodeTests.cpp.
// Round 4: Private/Handlers/GameplayHandlers_BTAuthoring.cpp and
// Private/Tests/BehaviorTreeAuthoringTests.cpp.
