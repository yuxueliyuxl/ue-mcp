#include "DatasmithHandlers.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"
#include "HandlerFunctionCall.h"
#include "HandlerJsonProperty.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorFramework/AssetImportData.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

// Every Datasmith type is reached by its reflected path, so the bridge builds
// and loads with the plugins disabled.
namespace UEMCP::Datasmith
{
	const TCHAR* const SceneElementClass = TEXT("/Script/DatasmithImporter.DatasmithSceneElement");
	const TCHAR* const ImportOptionsClass = TEXT("/Script/DatasmithContent.DatasmithImportOptions");
	const TCHAR* const SceneActorClass = TEXT("/Script/DatasmithContent.DatasmithSceneActor");
	const TCHAR* const SceneAssetClass = TEXT("/Script/DatasmithContent.DatasmithScene");
	const TCHAR* const ContentLibraryClass = TEXT("/Script/DatasmithContent.DatasmithContentBlueprintLibrary");

	bool IsPluginEnabled(const TCHAR* Name)
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Name);
		return Plugin.IsValid() && Plugin->IsEnabled();
	}

	UClass* FindClass(const TCHAR* Path)
	{
		return FindObject<UClass>(nullptr, Path);
	}

	/** Null when the importer is usable; otherwise the error to return. */
	TSharedPtr<FJsonValue> RequireImporter()
	{
		if (FindClass(SceneElementClass) && FindClass(SceneActorClass)) return nullptr;
		return MCPErrorWithCode(TEXT("PLUGIN_NOT_AVAILABLE"), FString::Printf(
			TEXT("Datasmith plugin not available: the DatasmithImporter plugin is %s in this project. Enable it with project(enable_plugin, pluginName=\"DatasmithImporter\") and restart the editor."),
			IsPluginEnabled(TEXT("DatasmithImporter")) ? TEXT("enabled but its module is not loaded") : TEXT("not enabled")));
	}

	TSharedPtr<FJsonValue> RequireContent()
	{
		if (FindClass(SceneActorClass) && FindClass(ContentLibraryClass)) return nullptr;
		return MCPErrorWithCode(TEXT("PLUGIN_NOT_AVAILABLE"), TEXT(
			"Datasmith plugin not available: DatasmithContent is not loaded, so Datasmith metadata cannot be read. Enable the DatasmithImporter plugin with project(enable_plugin) and restart the editor."));
	}

	FProperty* FindProp(const UObject* Obj, const TCHAR* Name)
	{
		return Obj ? Obj->GetClass()->FindPropertyByName(FName(Name)) : nullptr;
	}

	UObject* GetObjectProp(UObject* Obj, const TCHAR* Name)
	{
		FObjectPropertyBase* Prop = CastField<FObjectPropertyBase>(FindProp(Obj, Name));
		return Prop ? Prop->GetObjectPropertyValue_InContainer(Obj) : nullptr;
	}

	int32 MapPropNum(UObject* Obj, const TCHAR* Name)
	{
		FMapProperty* Prop = CastField<FMapProperty>(FindProp(Obj, Name));
		if (!Prop) return 0;
		FScriptMapHelper Helper(Prop, Prop->ContainerPtrToValuePtr<void>(Obj));
		return Helper.Num();
	}

	FString ReturnString(const FMCPReflectedCall& Call)
	{
		FStrProperty* Prop = CastField<FStrProperty>(Call.ReturnParam());
		return Prop ? Prop->GetPropertyValue_InContainer(Call.Frame.GetData()) : FString();
	}

	int32 ReturnArrayNum(const FMCPReflectedCall& Call)
	{
		FArrayProperty* Prop = CastField<FArrayProperty>(Call.ReturnParam());
		if (!Prop) return 0;
		FScriptArrayHelper Helper(Prop, Prop->ContainerPtrToValuePtr<void>(const_cast<uint8*>(Call.Frame.GetData())));
		return Helper.Num();
	}

	/** Structs as nested objects, everything else through the shared marshaller. */
	TSharedPtr<FJsonObject> StructToJson(const UStruct* Struct, const void* Data, UObject* Parent, int32 Depth = 0)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* Prop = *It;
			if (Prop->GetOwnerClass() == UObject::StaticClass()) continue;
			const void* Value = Prop->ContainerPtrToValuePtr<void>(Data);
			FStructProperty* StructProp = CastField<FStructProperty>(Prop);
			if (StructProp && StructProp->Struct && Depth < 4)
			{
				Obj->SetObjectField(Prop->GetName(), StructToJson(StructProp->Struct, Value, Parent, Depth + 1));
			}
			else
			{
				Obj->SetField(Prop->GetName(), MCPFunctionCall::ValueToJson(Prop, Value, Parent));
			}
		}
		return Obj;
	}

	/** Every options object a scene element carries, keyed by class name. */
	TMap<FString, UObject*> AllOptions(UObject* Scene)
	{
		TMap<FString, UObject*> Out;
		FMCPReflectedCall Call;
		FString Err;
		if (!Call.BindInstance(Scene, TEXT("GetAllOptions"), Err)) return Out;
		Call.Invoke();
		FMapProperty* Ret = CastField<FMapProperty>(Call.ReturnParam());
		if (!Ret) return Out;
		FScriptMapHelper Helper(Ret, Ret->ContainerPtrToValuePtr<void>(Call.Frame.GetData()));
		FObjectPropertyBase* ValueProp = CastField<FObjectPropertyBase>(Ret->ValueProp);
		if (!ValueProp) return Out;
		for (int32 i = 0, Max = Helper.GetMaxIndex(); i < Max; ++i)
		{
			if (!Helper.IsValidIndex(i)) continue;
			if (UObject* Options = ValueProp->GetObjectPropertyValue(Helper.GetValuePtr(i)))
			{
				Out.Add(Options->GetClass()->GetName(), Options);
			}
		}
		return Out;
	}

	UObject* ImportOptions(UObject* Scene)
	{
		UClass* OptionsClass = FindClass(ImportOptionsClass);
		FMCPReflectedCall Call;
		FString Err;
		if (!OptionsClass || !Call.BindInstance(Scene, TEXT("GetOptions"), Err)) return nullptr;
		Call.SetObject(TEXT("OptionType"), OptionsClass);
		Call.Invoke();
		return Call.ReturnObject();
	}

	/**
	 * Apply {dotted.path: value} to the scene's options. A key whose first
	 * segment names an options class (DatasmithCommonTessellationOptions.Options.ChordTolerance)
	 * targets that object; every other key targets DatasmithImportOptions.
	 */
	TSharedPtr<FJsonValue> ApplyOptions(UObject* Scene, const TSharedPtr<FJsonObject>& Options, TArray<FString>& OutApplied)
	{
		if (!Options.IsValid() || Options->Values.Num() == 0) return nullptr;
		const TMap<FString, UObject*> All = AllOptions(Scene);
		UObject* Default = ImportOptions(Scene);
		for (const auto& Pair : Options->Values)
		{
			const FString Key(Pair.Key);
			FString Head, Rest;
			UObject* Target = Default;
			FString Path = Key;
			if (Key.Split(TEXT("."), &Head, &Rest))
			{
				for (const TPair<FString, UObject*>& Entry : All)
				{
					if (Entry.Key.Equals(Head, ESearchCase::IgnoreCase) || Entry.Key.Equals(TEXT("U") + Head, ESearchCase::IgnoreCase))
					{
						Target = Entry.Value;
						Path = Rest;
						break;
					}
				}
			}
			if (!Target)
			{
				return MCPError(TEXT("The scene has no DatasmithImportOptions to apply options to"));
			}
			FString Err;
			if (!MCPJsonProperty::SetDottedPropertyFromJson(Target, Path, Pair.Value, Err))
			{
				TArray<FString> Names;
				All.GetKeys(Names);
				return MCPError(FString::Printf(TEXT("options['%s']: %s. Option objects on this scene: %s"),
					*Key, *Err, *FString::Join(Names, TEXT(", "))));
			}
			OutApplied.Add(Key);
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> SourceCounts(UObject* Scene)
	{
		TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
		static const TPair<const TCHAR*, const TCHAR*> Getters[] = {
			{ TEXT("meshes"), TEXT("GetMeshes") },
			{ TEXT("meshActors"), TEXT("GetAllMeshActors") },
			{ TEXT("lights"), TEXT("GetAllLightActors") },
			{ TEXT("cameras"), TEXT("GetAllCameraActors") },
			{ TEXT("customActors"), TEXT("GetAllCustomActors") },
			{ TEXT("materials"), TEXT("GetAllMaterials") },
			{ TEXT("textures"), TEXT("GetTextures") },
			{ TEXT("levelVariantSets"), TEXT("GetAllLevelVariantSets") },
		};
		for (const TPair<const TCHAR*, const TCHAR*>& Getter : Getters)
		{
			FMCPReflectedCall Call;
			FString Err;
			if (!Call.BindInstance(Scene, Getter.Value, Err)) continue;
			Call.Invoke();
			Counts->SetNumberField(Getter.Key, ReturnArrayNum(Call));
		}
		return Counts;
	}

	int32 TotalCount(const TSharedPtr<FJsonObject>& Counts)
	{
		int32 Total = 0;
		for (const auto& Pair : Counts->Values) Total += (int32)Pair.Value->AsNumber();
		return Total;
	}

	/** Open a .udatasmith (or other translatable) file as a scene element, translated. */
	UObject* OpenSceneFile(const FString& FilePath, bool& bOutTranslated, TSharedPtr<FJsonValue>& OutError)
	{
		bOutTranslated = false;
		FMCPReflectedCall Construct;
		FString Err;
		if (!Construct.Bind(SceneElementClass, TEXT("ConstructDatasmithSceneFromFile"), Err))
		{
			OutError = MCPError(Err);
			return nullptr;
		}
		Construct.SetString(TEXT("FilePath"), FilePath);
		Construct.Invoke();
		UObject* Scene = Construct.ReturnObject();
		if (!Scene)
		{
			OutError = MCPError(FString::Printf(TEXT("Datasmith could not open '%s'. Is it a file a Datasmith translator handles (.udatasmith, CAD formats with their plugin)?"), *FilePath));
			return nullptr;
		}
		// An element that arrives empty has not been through translation yet.
		if (TotalCount(SourceCounts(Scene)) == 0)
		{
			FMCPReflectedCall Translate;
			if (Translate.BindInstance(Scene, TEXT("TranslateScene"), Err))
			{
				Translate.Invoke();
				bOutTranslated = Translate.BoolReturn();
			}
		}
		return Scene;
	}

	void DestroyScene(UObject* Scene)
	{
		FMCPReflectedCall Call;
		FString Err;
		if (Scene && Call.BindInstance(Scene, TEXT("DestroyScene"), Err)) Call.Invoke();
	}

	TSharedPtr<FJsonObject> SceneHeader(UObject* Scene)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		static const TPair<const TCHAR*, const TCHAR*> Getters[] = {
			{ TEXT("host"), TEXT("GetHost") },
			{ TEXT("vendor"), TEXT("GetVendor") },
			{ TEXT("productName"), TEXT("GetProductName") },
			{ TEXT("productVersion"), TEXT("GetProductVersion") },
			{ TEXT("exporterVersion"), TEXT("GetExporterVersion") },
		};
		for (const TPair<const TCHAR*, const TCHAR*>& Getter : Getters)
		{
			FMCPReflectedCall Call;
			FString Err;
			if (!Call.BindInstance(Scene, Getter.Value, Err)) continue;
			Call.Invoke();
			Obj->SetStringField(Getter.Key, ReturnString(Call));
		}
		return Obj;
	}

	/** Asset counts of a DatasmithScene asset, read from its reflected maps. */
	TSharedPtr<FJsonObject> SceneAssetCounts(UObject* SceneAsset)
	{
		TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
		Counts->SetNumberField(TEXT("staticMeshes"), MapPropNum(SceneAsset, TEXT("StaticMeshes")));
		Counts->SetNumberField(TEXT("materials"), MapPropNum(SceneAsset, TEXT("Materials")));
		Counts->SetNumberField(TEXT("textures"), MapPropNum(SceneAsset, TEXT("Textures")));
		Counts->SetNumberField(TEXT("materialFunctions"), MapPropNum(SceneAsset, TEXT("MaterialFunctions")));
		Counts->SetNumberField(TEXT("levelSequences"), MapPropNum(SceneAsset, TEXT("LevelSequences")));
		Counts->SetNumberField(TEXT("levelVariantSets"), MapPropNum(SceneAsset, TEXT("LevelVariantSets")));
		return Counts;
	}

	struct FRelatedActor
	{
		FString Path;
		FString Label;
		FTransform Transform;
		bool bAlive = false;
	};

	/** RelatedActors of every DatasmithSceneActor in the world whose Scene is this asset, by Datasmith id. */
	TMap<FName, FRelatedActor> RelatedActorsOf(UWorld* World, UObject* SceneAsset, TArray<AActor*>* OutSceneActors = nullptr)
	{
		TMap<FName, FRelatedActor> Out;
		UClass* ActorClass = FindClass(SceneActorClass);
		if (!World || !ActorClass) return Out;
		for (TActorIterator<AActor> It(World, ActorClass); It; ++It)
		{
			AActor* SceneActor = *It;
			if (SceneAsset && GetObjectProp(SceneActor, TEXT("Scene")) != SceneAsset) continue;
			if (OutSceneActors) OutSceneActors->Add(SceneActor);
			FMapProperty* MapProp = CastField<FMapProperty>(FindProp(SceneActor, TEXT("RelatedActors")));
			if (!MapProp) continue;
			FNameProperty* KeyProp = CastField<FNameProperty>(MapProp->KeyProp);
			FSoftObjectProperty* ValueProp = CastField<FSoftObjectProperty>(MapProp->ValueProp);
			if (!KeyProp || !ValueProp) continue;
			FScriptMapHelper Helper(MapProp, MapProp->ContainerPtrToValuePtr<void>(SceneActor));
			for (int32 i = 0, Max = Helper.GetMaxIndex(); i < Max; ++i)
			{
				if (!Helper.IsValidIndex(i)) continue;
				const FName Key = KeyProp->GetPropertyValue(Helper.GetKeyPtr(i));
				const FSoftObjectPtr& Soft = ValueProp->GetPropertyValue(Helper.GetValuePtr(i));
				FRelatedActor Entry;
				Entry.Path = Soft.ToString();
				if (AActor* Related = Cast<AActor>(Soft.Get()))
				{
					Entry.bAlive = IsValid(Related);
					Entry.Label = Related->GetActorLabel();
					Entry.Transform = Related->GetActorTransform();
				}
				Out.Add(Key, Entry);
			}
		}
		return Out;
	}

	TSharedPtr<FJsonObject> DescribeActorRef(AActor* Actor)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("actorLabel"), Actor->GetActorLabel());
		Obj->SetStringField(TEXT("actorPath"), Actor->GetPathName());
		Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
		return Obj;
	}

	/** The Datasmith metadata on an actor or component, or null when it carries none. */
	TSharedPtr<FJsonObject> UserDataMetadata(UObject* Object)
	{
		FMCPReflectedCall Call;
		FString Err;
		if (!Call.Bind(ContentLibraryClass, TEXT("GetDatasmithUserData"), Err)) return nullptr;
		Call.SetObject(TEXT("Object"), Object);
		Call.Invoke();
		UObject* UserData = Call.ReturnObject();
		if (!UserData) return nullptr;
		FMapProperty* MapProp = CastField<FMapProperty>(FindProp(UserData, TEXT("MetaData")));
		if (!MapProp) return nullptr;
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		FScriptMapHelper Helper(MapProp, MapProp->ContainerPtrToValuePtr<void>(UserData));
		for (int32 i = 0, Max = Helper.GetMaxIndex(); i < Max; ++i)
		{
			if (!Helper.IsValidIndex(i)) continue;
			FString Key;
			MapProp->KeyProp->ExportTextItem_Direct(Key, Helper.GetKeyPtr(i), nullptr, nullptr, PPF_None);
			const FString* Value = reinterpret_cast<const FString*>(Helper.GetValuePtr(i));
			if (CastField<FStrProperty>(MapProp->ValueProp)) Out->SetStringField(Key, *Value);
		}
		return Out;
	}

	bool ValidDestination(const FString& Path)
	{
		return Path.StartsWith(TEXT("/")) && FPackageName::IsValidLongPackageName(Path / TEXT("Probe"));
	}
}

// ─── Handlers ────────────────────────────────────────────────────────────────

TSharedPtr<FJsonValue> FDatasmithHandlers::Status(const TSharedPtr<FJsonObject>& Params)
{
	using namespace UEMCP::Datasmith;
	auto Result = MCPSuccess();
	Result->SetBoolField(TEXT("importerPluginEnabled"), IsPluginEnabled(TEXT("DatasmithImporter")));
	Result->SetBoolField(TEXT("contentPluginEnabled"), IsPluginEnabled(TEXT("DatasmithContent")));
	Result->SetBoolField(TEXT("importerLoaded"), FindClass(SceneElementClass) != nullptr);
	Result->SetBoolField(TEXT("contentLoaded"), FindClass(SceneActorClass) != nullptr);
	Result->SetBoolField(TEXT("available"), !RequireImporter().IsValid());

	TArray<TSharedPtr<FJsonValue>> SceneActors;
	if (UWorld* World = GetEditorWorld())
	{
		TArray<AActor*> Found;
		RelatedActorsOf(World, nullptr, &Found);
		for (AActor* SceneActor : Found)
		{
			TSharedPtr<FJsonObject> Obj = DescribeActorRef(SceneActor);
			UObject* Asset = GetObjectProp(SceneActor, TEXT("Scene"));
			Obj->SetStringField(TEXT("sceneAsset"), Asset ? Asset->GetPathName() : FString());
			Obj->SetNumberField(TEXT("relatedActorCount"), MapPropNum(SceneActor, TEXT("RelatedActors")));
			SceneActors.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}
	Result->SetArrayField(TEXT("sceneActors"), SceneActors);
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FDatasmithHandlers::Inspect(const TSharedPtr<FJsonObject>& Params)
{
	using namespace UEMCP::Datasmith;
	FString FilePath;
	TSharedPtr<FJsonValue> PathErr = RequireString(Params, TEXT("filePath"), FilePath);

	if (TSharedPtr<FJsonValue> Err = RequireImporter()) return Err;
	if (PathErr) return PathErr;
	FilePath = FPaths::ConvertRelativePathToFull(FilePath);
	if (!FPaths::FileExists(FilePath)) return MCPError(FString::Printf(TEXT("No file at '%s'"), *FilePath));

	bool bTranslated = false;
	TSharedPtr<FJsonValue> OpenErr;
	UObject* Scene = OpenSceneFile(FilePath, bTranslated, OpenErr);
	if (!Scene) return OpenErr;

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("filePath"), FilePath);
	Result->SetObjectField(TEXT("scene"), SceneHeader(Scene));
	Result->SetObjectField(TEXT("counts"), SourceCounts(Scene));
	Result->SetBoolField(TEXT("translatedHere"), bTranslated);
	TSharedPtr<FJsonObject> OptionsJson = MakeShared<FJsonObject>();
	for (const TPair<FString, UObject*>& Entry : AllOptions(Scene))
	{
		OptionsJson->SetObjectField(Entry.Key, StructToJson(Entry.Value->GetClass(), Entry.Value, Entry.Value));
	}
	Result->SetObjectField(TEXT("options"), OptionsJson);
	Result->SetStringField(TEXT("optionsNote"), TEXT("Pass any of these to datasmith(import) as options {\"BaseOptions.bIncludeLight\": false}; a key starting with another options class name targets that object."));
	DestroyScene(Scene);
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FDatasmithHandlers::Import(const TSharedPtr<FJsonObject>& Params)
{
	using namespace UEMCP::Datasmith;
	FString FilePath, Destination;
	TSharedPtr<FJsonValue> PathErr = RequireString(Params, TEXT("filePath"), FilePath);
	TSharedPtr<FJsonValue> DestErr = RequireString(Params, TEXT("destinationPath"), Destination);
	const TSharedPtr<FJsonObject>* OptionsParam = nullptr;
	TryGetObjectParam(Params, TEXT("options"), OptionsParam);

	if (TSharedPtr<FJsonValue> Err = RequireImporter()) return Err;
	if (PathErr) return PathErr;
	if (DestErr) return DestErr;
	if (auto PieErr = MCPRefuseDuringPlayInEditor(TEXT("datasmith_import"))) return PieErr;
	FilePath = FPaths::ConvertRelativePathToFull(FilePath);
	if (!FPaths::FileExists(FilePath)) return MCPError(FString::Printf(TEXT("No file at '%s'"), *FilePath));
	Destination.RemoveFromEnd(TEXT("/"));
	if (!ValidDestination(Destination)) return MCPError(FString::Printf(TEXT("destinationPath '%s' is not a content folder such as /Game/Datasmith/Site"), *Destination));
	if (MCPIsProtectedAssetPath(Destination)) return MCPProtectedPathError(Destination);
	REQUIRE_EDITOR_WORLD(World);

	bool bTranslated = false;
	TSharedPtr<FJsonValue> OpenErr;
	UObject* Scene = OpenSceneFile(FilePath, bTranslated, OpenErr);
	if (!Scene) return OpenErr;
	const TSharedPtr<FJsonObject> Source = SourceCounts(Scene);

	TArray<FString> Applied;
	if (TSharedPtr<FJsonValue> OptErr = ApplyOptions(Scene, OptionsParam ? *OptionsParam : nullptr, Applied))
	{
		DestroyScene(Scene);
		return OptErr;
	}

	FMCPReflectedCall Call;
	FString Err;
	if (!Call.BindInstance(Scene, TEXT("ImportScene"), Err))
	{
		DestroyScene(Scene);
		return MCPError(Err);
	}
	Call.SetString(TEXT("DestinationFolder"), Destination);
	FMCPScopedLogCapture Log({ TEXT("LogDatasmith") });
	Call.Invoke();

	FStructProperty* Ret = CastField<FStructProperty>(Call.ReturnParam());
	if (!Ret || !Ret->Struct) return MCPError(TEXT("ImportScene returned no result struct in this engine build"));
	void* RetPtr = Ret->ContainerPtrToValuePtr<void>(Call.Frame.GetData());
	auto Field = [&](const TCHAR* Name) { return Ret->Struct->FindPropertyByName(FName(Name)); };
	bool bSucceeded = false;
	if (FBoolProperty* Ok = CastField<FBoolProperty>(Field(TEXT("bImportSucceed")))) bSucceeded = Ok->GetPropertyValue_InContainer(RetPtr);
	UObject* SceneAsset = nullptr;
	if (FObjectPropertyBase* P = CastField<FObjectPropertyBase>(Field(TEXT("Scene")))) SceneAsset = P->GetObjectPropertyValue_InContainer(RetPtr);
	TArray<AActor*> ImportedActors;
	int32 ImportedMeshes = 0;
	if (FArrayProperty* Arr = CastField<FArrayProperty>(Field(TEXT("ImportedActors"))))
	{
		FScriptArrayHelper Helper(Arr, Arr->ContainerPtrToValuePtr<void>(RetPtr));
		FObjectPropertyBase* Inner = CastField<FObjectPropertyBase>(Arr->Inner);
		for (int32 i = 0; Inner && i < Helper.Num(); ++i)
		{
			if (AActor* A = Cast<AActor>(Inner->GetObjectPropertyValue(Helper.GetRawPtr(i)))) ImportedActors.Add(A);
		}
	}
	if (FArrayProperty* Arr = CastField<FArrayProperty>(Field(TEXT("ImportedMeshes"))))
	{
		FScriptArrayHelper Helper(Arr, Arr->ContainerPtrToValuePtr<void>(RetPtr));
		ImportedMeshes = Helper.Num();
	}

	if (!bSucceeded)
	{
		TSharedPtr<FJsonObject> ErrObj = MCPErrorObject(FString::Printf(TEXT("Datasmith import of '%s' into %s did not succeed; warnings holds what the importer logged."), *FilePath, *Destination));
		ErrObj->SetObjectField(TEXT("sourceCounts"), Source);
		ErrObj->SetArrayField(TEXT("warnings"), Log.ProblemsJson());
		return MCPResult(ErrObj);
	}

	UClass* SceneActorCls = FindClass(SceneActorClass);
	AActor* SceneActor = nullptr;
	for (AActor* A : ImportedActors)
	{
		if (SceneActorCls && A->IsA(SceneActorCls)) { SceneActor = A; break; }
	}

	// The import result leaves its Scene field empty on some paths while the scene actor holds the
	// asset it created, so the actor is the fallback for the path reimport needs.
	if (!SceneAsset && SceneActor) SceneAsset = GetObjectProp(SceneActor, TEXT("Scene"));

	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("filePath"), FilePath);
	Result->SetStringField(TEXT("destinationPath"), Destination);
	Result->SetStringField(TEXT("sceneAsset"), SceneAsset ? SceneAsset->GetPathName() : FString());
	if (SceneActor)
	{
		TSharedPtr<FJsonObject> ActorObj = DescribeActorRef(SceneActor);
		ActorObj->SetNumberField(TEXT("relatedActorCount"), MapPropNum(SceneActor, TEXT("RelatedActors")));
		Result->SetObjectField(TEXT("sceneActor"), ActorObj);
	}
	TSharedPtr<FJsonObject> Counts = SceneAsset ? SceneAssetCounts(SceneAsset) : MakeShared<FJsonObject>();
	Counts->SetNumberField(TEXT("actors"), SceneActor ? MapPropNum(SceneActor, TEXT("RelatedActors")) : ImportedActors.Num());
	Counts->SetNumberField(TEXT("importedMeshes"), ImportedMeshes);
	Result->SetObjectField(TEXT("counts"), Counts);
	Result->SetObjectField(TEXT("sourceCounts"), Source);
	Result->SetArrayField(TEXT("optionsApplied"), MCPStringListToJson(Applied));
	Result->SetArrayField(TEXT("warnings"), Log.ProblemsJson());
	if (!SceneActor)
	{
		Result->SetStringField(TEXT("note"), TEXT("No DatasmithSceneActor was spawned, which is what BaseOptions.SceneHandling AssetsOnly does. datasmith(reimport) still works on the scene asset."));
	}
	MCPSetNoRollback(Result, TEXT("An import creates the scene asset, every mesh, material and texture it references, and the level actors; no single call removes that set. asset(delete) on destinationPath and deleting the scene actor undo it by hand."));
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FDatasmithHandlers::Reimport(const TSharedPtr<FJsonObject>& Params)
{
	using namespace UEMCP::Datasmith;
	FString SceneAssetPath;
	TSharedPtr<FJsonValue> PathErr = RequireString(Params, TEXT("sceneAssetPath"), SceneAssetPath);
	const FString NewFilePath = OptionalString(Params, TEXT("filePath"));
	const bool bSyncActors = OptionalBool(Params, TEXT("syncActors"), true);
	const bool bRespawnDeleted = OptionalBool(Params, TEXT("respawnDeleted"), false);
	const TSharedPtr<FJsonObject>* OptionsParam = nullptr;
	TryGetObjectParam(Params, TEXT("options"), OptionsParam);

	if (TSharedPtr<FJsonValue> Err = RequireImporter()) return Err;
	if (PathErr) return PathErr;
	if (auto PieErr = MCPRefuseDuringPlayInEditor(TEXT("datasmith_reimport"))) return PieErr;
	REQUIRE_EDITOR_WORLD(World);

	UObject* SceneAsset = LoadAssetByPath<UObject>(SceneAssetPath);
	UClass* AssetClass = FindClass(SceneAssetClass);
	if (!SceneAsset || !AssetClass || !SceneAsset->IsA(AssetClass)) return MCPAssetLoadError(SceneAssetPath, TEXT("DatasmithScene"));

	// A new source path goes on the asset's import data, which is what the reimport reads.
	FString PreviousSource, NewSource;
	if (!NewFilePath.IsEmpty())
	{
		const FString Full = FPaths::ConvertRelativePathToFull(NewFilePath);
		if (!FPaths::FileExists(Full)) return MCPError(FString::Printf(TEXT("No file at '%s'"), *Full));
		UAssetImportData* ImportData = Cast<UAssetImportData>(GetObjectProp(SceneAsset, TEXT("AssetImportData")));
		if (!ImportData) return MCPError(TEXT("The scene asset has no import data to point at a new source"));
		SceneAsset->Modify();
		ImportData->Modify();
		PreviousSource = ImportData->GetFirstFilename();
		ImportData->Update(Full);
		if (FStructProperty* InfoProp = CastField<FStructProperty>(FindProp(ImportData, TEXT("DatasmithImportInfo"))))
		{
			void* InfoPtr = InfoProp->ContainerPtrToValuePtr<void>(ImportData);
			if (FStrProperty* UriProp = CastField<FStrProperty>(InfoProp->Struct->FindPropertyByName(TEXT("SourceUri"))))
			{
				FString& Uri = *UriProp->ContainerPtrToValuePtr<FString>(InfoPtr);
				const FString OldNormalized = FPaths::ConvertRelativePathToFull(PreviousSource);
				if (!PreviousSource.IsEmpty() && Uri.EndsWith(OldNormalized, ESearchCase::IgnoreCase))
				{
					Uri = Uri.LeftChop(OldNormalized.Len()) + Full;
				}
				else if (!PreviousSource.IsEmpty() && Uri.EndsWith(PreviousSource, ESearchCase::IgnoreCase))
				{
					Uri = Uri.LeftChop(PreviousSource.Len()) + Full;
				}
				else
				{
					Uri = TEXT("file://") + Full;
				}
			}
			if (FStrProperty* HashProp = CastField<FStrProperty>(InfoProp->Struct->FindPropertyByName(TEXT("SourceHash"))))
			{
				HashProp->ContainerPtrToValuePtr<FString>(InfoPtr)->Empty();
			}
		}
		NewSource = Full;
	}

	FMCPReflectedCall Open;
	FString Err;
	if (!Open.Bind(SceneElementClass, TEXT("GetExistingDatasmithScene"), Err)) return MCPError(Err);
	Open.SetString(TEXT("AssetPath"), SceneAsset->GetOutermost()->GetName());
	Open.Invoke();
	UObject* Scene = Open.ReturnObject();
	if (!Scene) return MCPError(FString::Printf(TEXT("Datasmith could not open the scene asset '%s' for reimport"), *SceneAsset->GetPathName()));

	UObject* Options = ImportOptions(Scene);
	if (!Options) return MCPError(TEXT("The scene has no DatasmithImportOptions; it cannot be reimported here"));
	if (FStructProperty* ReimportProp = CastField<FStructProperty>(FindProp(Options, TEXT("ReimportOptions"))))
	{
		void* Ptr = ReimportProp->ContainerPtrToValuePtr<void>(Options);
		if (FBoolProperty* P = CastField<FBoolProperty>(ReimportProp->Struct->FindPropertyByName(TEXT("bUpdateActors")))) P->SetPropertyValue_InContainer(Ptr, bSyncActors);
		if (FBoolProperty* P = CastField<FBoolProperty>(ReimportProp->Struct->FindPropertyByName(TEXT("bRespawnDeletedActors")))) P->SetPropertyValue_InContainer(Ptr, bRespawnDeleted);
	}
	TArray<FString> Applied;
	if (TSharedPtr<FJsonValue> OptErr = ApplyOptions(Scene, OptionsParam ? *OptionsParam : nullptr, Applied)) return OptErr;

	const TMap<FName, FRelatedActor> Before = RelatedActorsOf(World, SceneAsset);
	const TSharedPtr<FJsonObject> CountsBefore = SceneAssetCounts(SceneAsset);

	FMCPReflectedCall Call;
	if (!Call.BindInstance(Scene, TEXT("ReimportScene"), Err)) return MCPError(Err);
	FMCPScopedLogCapture Log({ TEXT("LogDatasmith") });
	Call.Invoke();
	bool bSucceeded = false;
	if (FStructProperty* Ret = CastField<FStructProperty>(Call.ReturnParam()))
	{
		if (FBoolProperty* Ok = CastField<FBoolProperty>(Ret->Struct->FindPropertyByName(TEXT("bImportSucceed"))))
		{
			bSucceeded = Ok->GetPropertyValue_InContainer(Ret->ContainerPtrToValuePtr<void>(Call.Frame.GetData()));
		}
	}
	if (!bSucceeded)
	{
		TSharedPtr<FJsonObject> ErrObj = MCPErrorObject(FString::Printf(TEXT("Datasmith reimport of '%s' did not succeed; warnings holds what the importer logged."), *SceneAsset->GetPathName()));
		ErrObj->SetArrayField(TEXT("warnings"), Log.ProblemsJson());
		return MCPResult(ErrObj);
	}

	// Per-actor changes, by Datasmith id, across every scene actor of this asset.
	const TMap<FName, FRelatedActor> After = RelatedActorsOf(World, SceneAsset);
	constexpr int32 MaxListed = 50;
	TArray<TSharedPtr<FJsonValue>> Added, Removed, Moved;
	int32 AddedCount = 0, RemovedCount = 0, MovedCount = 0, Kept = 0;
	for (const TPair<FName, FRelatedActor>& Pair : After)
	{
		const FRelatedActor* Old = Before.Find(Pair.Key);
		if (!Old || (!Old->bAlive && Pair.Value.bAlive))
		{
			if (Pair.Value.bAlive && ++AddedCount <= MaxListed) Added.Add(MakeShared<FJsonValueString>(Pair.Value.Label.IsEmpty() ? Pair.Key.ToString() : Pair.Value.Label));
			continue;
		}
		if (Old->bAlive && !Pair.Value.bAlive)
		{
			if (++RemovedCount <= MaxListed) Removed.Add(MakeShared<FJsonValueString>(Old->Label.IsEmpty() ? Pair.Key.ToString() : Old->Label));
			continue;
		}
		++Kept;
		if (Old->bAlive && Pair.Value.bAlive && !Old->Transform.Equals(Pair.Value.Transform, 0.01))
		{
			if (++MovedCount <= MaxListed) Moved.Add(MakeShared<FJsonValueString>(Pair.Value.Label));
		}
	}
	for (const TPair<FName, FRelatedActor>& Pair : Before)
	{
		if (!After.Contains(Pair.Key) && Pair.Value.bAlive && ++RemovedCount <= MaxListed)
		{
			Removed.Add(MakeShared<FJsonValueString>(Pair.Value.Label.IsEmpty() ? Pair.Key.ToString() : Pair.Value.Label));
		}
	}

	auto Result = MCPSuccess();
	MCPSetUpdated(Result);
	Result->SetStringField(TEXT("sceneAsset"), SceneAsset->GetPathName());
	if (!NewSource.IsEmpty())
	{
		Result->SetStringField(TEXT("previousSource"), PreviousSource);
		Result->SetStringField(TEXT("source"), NewSource);
	}
	Result->SetBoolField(TEXT("syncActors"), bSyncActors);
	Result->SetBoolField(TEXT("respawnDeleted"), bRespawnDeleted);
	Result->SetObjectField(TEXT("countsBefore"), CountsBefore);
	Result->SetObjectField(TEXT("counts"), SceneAssetCounts(SceneAsset));
	TSharedPtr<FJsonObject> Actors = MakeShared<FJsonObject>();
	Actors->SetNumberField(TEXT("before"), Before.Num());
	Actors->SetNumberField(TEXT("after"), After.Num());
	Actors->SetNumberField(TEXT("addedCount"), AddedCount);
	Actors->SetNumberField(TEXT("removedCount"), RemovedCount);
	Actors->SetNumberField(TEXT("movedCount"), MovedCount);
	Actors->SetNumberField(TEXT("keptCount"), Kept);
	Actors->SetArrayField(TEXT("added"), Added);
	Actors->SetArrayField(TEXT("removed"), Removed);
	Actors->SetArrayField(TEXT("moved"), Moved);
	Result->SetObjectField(TEXT("actorChanges"), Actors);
	Result->SetArrayField(TEXT("optionsApplied"), MCPStringListToJson(Applied));
	Result->SetArrayField(TEXT("warnings"), Log.ProblemsJson());
	MCPSetNoRollback(Result, TEXT("A reimport rewrites the scene's assets and its actors from the source; the previous asset data is not kept, so no call restores it."));
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FDatasmithHandlers::QueryActors(const TSharedPtr<FJsonObject>& Params)
{
	using namespace UEMCP::Datasmith;
	const FString TagPrefix = OptionalString(Params, TEXT("tagPrefix"));
	const TArray<TSharedPtr<FJsonValue>>* TagsParam = nullptr;
	TryGetArrayParam(Params, TEXT("tags"), TagsParam);
	const FString MetadataKey = OptionalString(Params, TEXT("metadataKey"));
	const FString MetadataValue = OptionalString(Params, TEXT("metadataValue"));
	const bool bIncludeMetadata = OptionalBool(Params, TEXT("includeMetadata"), true);
	const bool bIncludeBounds = OptionalBool(Params, TEXT("includeBounds"), true);
	const int32 Limit = FMath::Clamp(OptionalInt(Params, TEXT("limit"), 200), 1, 5000);

	const TArray<FString> Tags = JsonArrayToStringList(TagsParam);
	if (TagPrefix.IsEmpty() && Tags.Num() == 0 && MetadataKey.IsEmpty())
	{
		return MCPError(TEXT("Pass tagPrefix, tags or metadataKey to choose the actors"));
	}
	const bool bNeedMetadata = bIncludeMetadata || !MetadataKey.IsEmpty();
	const bool bMetadataReadable = !RequireContent().IsValid();
	if (!MetadataKey.IsEmpty() && !bMetadataReadable) return RequireContent();
	REQUIRE_EDITOR_WORLD(World);

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 Matched = 0;
	FBox Union(ForceInit);
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;
		TArray<FString> MatchedTags;
		for (const FName& Tag : Actor->Tags)
		{
			const FString T = Tag.ToString();
			if ((!TagPrefix.IsEmpty() && T.StartsWith(TagPrefix, ESearchCase::IgnoreCase))
				|| Tags.ContainsByPredicate([&T](const FString& W) { return W.Equals(T, ESearchCase::IgnoreCase); }))
			{
				MatchedTags.Add(T);
			}
		}
		const bool bTagFilter = !TagPrefix.IsEmpty() || Tags.Num() > 0;
		if (bTagFilter && MatchedTags.Num() == 0) continue;

		TSharedPtr<FJsonObject> Metadata;
		if (bNeedMetadata && bMetadataReadable)
		{
			Metadata = UserDataMetadata(Actor);
			if (!Metadata.IsValid() && Actor->GetRootComponent()) Metadata = UserDataMetadata(Actor->GetRootComponent());
		}
		if (!MetadataKey.IsEmpty())
		{
			FString Value;
			if (!Metadata.IsValid() || !Metadata->TryGetStringField(MetadataKey, Value)) continue;
			if (!MetadataValue.IsEmpty() && !Value.Equals(MetadataValue, ESearchCase::IgnoreCase)) continue;
		}

		++Matched;
		FVector Origin, Extent;
		Actor->GetActorBounds(false, Origin, Extent);
		if (!Extent.IsNearlyZero()) Union += FBox(Origin - Extent, Origin + Extent);
		if (Rows.Num() >= Limit) continue;

		TSharedPtr<FJsonObject> Row = DescribeActorRef(Actor);
		Row->SetArrayField(TEXT("matchedTags"), MCPStringListToJson(MatchedTags));
		if (bIncludeMetadata)
		{
			if (Metadata.IsValid()) Row->SetObjectField(TEXT("metadata"), Metadata);
			else Row->SetField(TEXT("metadata"), MakeShared<FJsonValueNull>());
		}
		if (bIncludeBounds)
		{
			TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
			B->SetObjectField(TEXT("center"), MCPVec3ToJsonObject(Origin));
			B->SetObjectField(TEXT("extent"), MCPVec3ToJsonObject(Extent));
			B->SetObjectField(TEXT("min"), MCPVec3ToJsonObject(Origin - Extent));
			B->SetObjectField(TEXT("max"), MCPVec3ToJsonObject(Origin + Extent));
			Row->SetObjectField(TEXT("bounds"), B);
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	auto Result = MCPSuccess();
	Result->SetArrayField(TEXT("actors"), Rows);
	Result->SetNumberField(TEXT("matchedCount"), Matched);
	Result->SetBoolField(TEXT("truncated"), Matched > Rows.Num());
	Result->SetBoolField(TEXT("metadataAvailable"), bMetadataReadable);
	if (bIncludeBounds && Union.IsValid)
	{
		TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
		B->SetObjectField(TEXT("min"), MCPVec3ToJsonObject(Union.Min));
		B->SetObjectField(TEXT("max"), MCPVec3ToJsonObject(Union.Max));
		Result->SetObjectField(TEXT("unionBounds"), B);
	}
	if (bIncludeMetadata && !bMetadataReadable)
	{
		Result->SetStringField(TEXT("note"), TEXT("Datasmith plugin not available, so metadata is omitted; tags and bounds are still reported."));
	}
	MCPNoteLoadedOnlyEnumeration(World, Result);
	return MCPResult(Result);
}

// ─── Registration ────────────────────────────────────────────────────────────

void FDatasmithHandlers::RegisterHandlers(FMCPHandlerRegistry& Registry)
{
	FMCPHandlerRegistry::FCategoryScope CategoryScope(Registry, TEXT("datasmith"));
	using EType = EMCPParamType;

	Registry.RegisterHandler(TEXT("datasmith_status"), &Status, {});
	Registry.RegisterHandlerWithTimeout(TEXT("datasmith_inspect"), &Inspect, 300.0f, {
		MCPParam::Required(TEXT("filePath"), EType::String, TEXT("Absolute path of the .udatasmith (or other Datasmith-translatable) file")),
	});
	Registry.RegisterHandlerWithTimeout(TEXT("datasmith_import"), &Import, 600.0f, {
		MCPParam::Required(TEXT("filePath"), EType::String, TEXT("Absolute path of the .udatasmith file to import")),
		MCPParam::Required(TEXT("destinationPath"), EType::String, TEXT("Content folder for the scene asset and its assets, e.g. /Game/Datasmith/Site")),
		MCPParam::Optional(TEXT("options"), EType::Object, TEXT("{dotted.option.path: value} on DatasmithImportOptions, e.g. {\"BaseOptions.bIncludeLight\": false, \"BaseOptions.SceneHandling\": \"AssetsOnly\"}; datasmith(inspect) lists them")),
	});
	Registry.RegisterHandlerWithTimeout(TEXT("datasmith_reimport"), &Reimport, 600.0f, {
		MCPParam::Required(TEXT("sceneAssetPath"), EType::String, TEXT("The DatasmithScene asset to reimport")),
		MCPParam::Optional(TEXT("filePath"), EType::String, TEXT("New source file path, when the source moved or changed name")),
		MCPParam::Optional(TEXT("syncActors"), EType::Boolean, TEXT("Update the scene's actors in the level from the new source (default true)")),
		MCPParam::Optional(TEXT("respawnDeleted"), EType::Boolean, TEXT("Respawn scene actors that were deleted from the level (default false)")),
		MCPParam::Optional(TEXT("options"), EType::Object, TEXT("{dotted.option.path: value} applied before the reimport")),
	});
	Registry.RegisterHandler(TEXT("datasmith_query_actors"), &QueryActors, {
		MCPParam::Optional(TEXT("tagPrefix"), EType::String, TEXT("Match actors with a tag starting with this, e.g. Revit.Element.Id.")),
		MCPParam::Optional(TEXT("tags"), EType::Array, TEXT("Match actors carrying any of these exact tags")).Items(EType::String),
		MCPParam::Optional(TEXT("metadataKey"), EType::String, TEXT("Match actors whose Datasmith metadata has this key")),
		MCPParam::Optional(TEXT("metadataValue"), EType::String, TEXT("With metadataKey, match only this value")),
		MCPParam::Optional(TEXT("includeMetadata"), EType::Boolean, TEXT("Return each actor's Datasmith user-data metadata (default true)")),
		MCPParam::Optional(TEXT("includeBounds"), EType::Boolean, TEXT("Return each actor's world bounds and the union (default true)")),
		MCPParam::Optional(TEXT("limit"), EType::Integer, TEXT("Rows to return (default 200, max 5000); matchedCount counts them all")),
	});
}
