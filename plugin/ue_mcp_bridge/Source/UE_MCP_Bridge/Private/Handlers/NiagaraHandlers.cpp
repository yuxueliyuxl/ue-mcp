#include "NiagaraHandlers.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"

#include "UObject/StrongObjectPtr.h"
#include "HandlerJsonProperty.h"
#include "HandlerAssetCreate.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "EditorScriptingUtilities/Public/EditorAssetLibrary.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/TopLevelAssetPath.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraComponent.h"
#include "NiagaraActor.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraDataInterface.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScript.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeInput.h"
#include "NiagaraDataInterfaceParticleRead.h"
#include "NiagaraNodeOp.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "NiagaraStackFunctionInputBinder.h"
#include "NiagaraParameterMapHistory.h"
#include "NiagaraTypes.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitterFactoryNew.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Factories/Factory.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphNode.h"

namespace
{
	UFactory* CreateNiagaraEditorFactoryByClassPath(const TCHAR* ClassPath)
	{
		UClass* FactoryClass = LoadObject<UClass>(nullptr, ClassPath);
		if (!FactoryClass || !FactoryClass->IsChildOf(UFactory::StaticClass()))
		{
			return nullptr;
		}
		return NewObject<UFactory>(GetTransientPackage(), FactoryClass);
	}
}

void FNiagaraHandlers::RegisterHandlers(FMCPHandlerRegistry& Registry)
{
	Registry.RegisterHandler(TEXT("list_niagara_systems"), &ListNiagaraSystems);
	Registry.RegisterHandler(TEXT("list_niagara_modules"), &ListNiagaraModules);
	Registry.RegisterHandler(TEXT("create_niagara_system"), &CreateNiagaraSystem);
	Registry.RegisterHandler(TEXT("get_niagara_info"), &GetNiagaraInfo);
	Registry.RegisterHandler(TEXT("list_emitters_in_system"), &ListEmittersInSystem);
	Registry.RegisterHandler(TEXT("create_niagara_emitter"), &CreateNiagaraEmitter);
	Registry.RegisterHandler(TEXT("spawn_niagara_at_location"), &SpawnNiagaraAtLocation);
	Registry.RegisterHandler(TEXT("spawn_niagara_actor"), &SpawnNiagaraActor);
	Registry.RegisterHandler(TEXT("reactivate_niagara"), &ReactivateNiagara);
	Registry.RegisterHandler(TEXT("set_niagara_parameter"), &SetNiagaraParameter);
	Registry.RegisterHandler(TEXT("add_emitter_to_system"), &AddEmitterToSystem);
	Registry.RegisterHandler(TEXT("set_emitter_property"), &SetEmitterProperty);
	Registry.RegisterHandler(TEXT("get_emitter_info"), &GetEmitterInfo);

	// v0.7.10 - depth
	Registry.RegisterHandler(TEXT("list_emitter_renderers"), &ListEmitterRenderers);
	Registry.RegisterHandler(TEXT("add_emitter_renderer"), &AddEmitterRenderer);
	Registry.RegisterHandler(TEXT("remove_emitter_renderer"), &RemoveEmitterRenderer);
	Registry.RegisterHandler(TEXT("set_renderer_property"), &SetRendererProperty);
	Registry.RegisterHandler(TEXT("inspect_data_interface"), &InspectDataInterface);
	Registry.RegisterHandler(TEXT("create_niagara_system_from_spec"), &CreateNiagaraSystemFromSpec);
	Registry.RegisterHandler(TEXT("get_niagara_compiled_hlsl"), &GetCompiledHLSL);
	Registry.RegisterHandler(TEXT("list_niagara_system_parameters"), &ListSystemParameters);

	// v0.7.14 - module inputs, static switches, HLSL modules
	Registry.RegisterHandler(TEXT("list_niagara_module_inputs"), &ListModuleInputs);
	Registry.RegisterHandler(TEXT("set_niagara_module_input"), &SetModuleInput);
	Registry.RegisterHandler(TEXT("add_niagara_module"), &AddModule);
	Registry.RegisterHandler(TEXT("remove_emitter_from_system"), &RemoveEmitterFromSystem);
	Registry.RegisterHandler(TEXT("validate_niagara_system"), &ValidateSystem);
	Registry.RegisterHandler(TEXT("list_niagara_static_switches"), &ListStaticSwitches);
	Registry.RegisterHandler(TEXT("set_niagara_static_switch"), &SetStaticSwitch);
	Registry.RegisterHandler(TEXT("create_niagara_module_from_hlsl"), &CreateModuleFromHlsl);
	// #185: Create an empty scratch-pad-style module
	Registry.RegisterHandler(TEXT("create_scratch_module"), &CreateScratchModule);
	// GPU-refactor support
	Registry.RegisterHandler(TEXT("describe_niagara_module_graph"), &DescribeModuleGraph);
	Registry.RegisterHandler(TEXT("set_niagara_module_enabled"), &SetModuleEnabled);
	Registry.RegisterHandler(TEXT("describe_niagara_script_graph"), &DescribeScriptGraph);
	Registry.RegisterHandler(TEXT("set_niagara_di_function"), &SetDiFunction);
	Registry.RegisterHandler(TEXT("graph_add_node"), &GraphAddNode);
	Registry.RegisterHandler(TEXT("graph_connect_pins"), &GraphConnectPins);
	Registry.RegisterHandler(TEXT("graph_disconnect_pins"), &GraphDisconnectPins);
	Registry.RegisterHandler(TEXT("graph_set_pin"), &GraphSetPin);
	Registry.RegisterHandler(TEXT("graph_set_node_property"), &GraphSetNodeProperty);
	Registry.RegisterHandler(TEXT("graph_remove_node"), &GraphRemoveNode);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::ListNiagaraSystems(const TSharedPtr<FJsonObject>& Params)
{
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TArray<FAssetData> Assets;
	AR.GetAssetsByClass(FTopLevelAssetPath(TEXT("/Script/Niagara"), TEXT("NiagaraSystem")), Assets, true);

	TArray<TSharedPtr<FJsonValue>> AssetArray;
	for (const FAssetData& Asset : Assets)
	{
		TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
		AssetObj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		AssetObj->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		AssetObj->SetStringField(TEXT("type"), TEXT("System"));
		AssetArray.Add(MakeShared<FJsonValueObject>(AssetObj));
	}

	// Also include emitter assets (#67)
	TArray<FAssetData> EmitterAssets;
	AR.GetAssetsByClass(FTopLevelAssetPath(TEXT("/Script/Niagara"), TEXT("NiagaraEmitter")), EmitterAssets, true);
	for (const FAssetData& Asset : EmitterAssets)
	{
		TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
		AssetObj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		AssetObj->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		AssetObj->SetStringField(TEXT("type"), TEXT("Emitter"));
		AssetArray.Add(MakeShared<FJsonValueObject>(AssetObj));
	}

	auto Result = MCPSuccess();
	Result->SetArrayField(TEXT("assets"), AssetArray);
	Result->SetNumberField(TEXT("count"), AssetArray.Num());
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::ListNiagaraModules(const TSharedPtr<FJsonObject>& Params)
{
	// Default 200 keeps response small; engine ships ~200 NiagaraScripts. Use
	// pathFilter to narrow results, or pass a higher limit for full sweep.
	const int32 Limit = OptionalInt(Params, TEXT("limit"), 200);
	const FString PathFilter = OptionalString(Params, TEXT("pathFilter"));

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TArray<FAssetData> Assets;
	AR.GetAssetsByClass(FTopLevelAssetPath(TEXT("/Script/Niagara"), TEXT("NiagaraScript")), Assets, true);

	TArray<TSharedPtr<FJsonValue>> AssetArray;
	for (const FAssetData& Asset : Assets)
	{
		if (AssetArray.Num() >= Limit) break;
		const FString PathStr = Asset.GetObjectPathString();
		if (!PathFilter.IsEmpty() && !PathStr.Contains(PathFilter)) continue;

		TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
		AssetObj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		AssetObj->SetStringField(TEXT("path"), PathStr);
		AssetArray.Add(MakeShared<FJsonValueObject>(AssetObj));
	}

	auto Result = MCPSuccess();
	Result->SetArrayField(TEXT("modules"), AssetArray);
	Result->SetNumberField(TEXT("count"), AssetArray.Num());
	Result->SetNumberField(TEXT("totalAvailable"), Assets.Num());
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::CreateNiagaraSystem(const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (auto Err = RequireString(Params, TEXT("name"), Name)) return Err;

	FString PackagePath = OptionalString(Params, TEXT("packagePath"), TEXT("/Game/VFX"));
	const FString OnConflict = OptionalString(Params, TEXT("onConflict"), TEXT("skip"));

	UClass* FactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraSystemFactoryNew"));
	UFactory* Factory = nullptr;
	if (FactoryClass)
	{
		Factory = Cast<UFactory>(NewObject<UObject>(GetTransientPackage(), FactoryClass));
	}

	auto Created = MCPCreateAssetIdempotent<UNiagaraSystem>(Name, PackagePath, OnConflict, TEXT("NiagaraSystem"), Factory);
	if (Created.EarlyReturn) return Created.EarlyReturn;

	UEditorAssetLibrary::SaveAsset(Created.Asset->GetPathName());

	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("path"), Created.Asset->GetPathName());
	Result->SetStringField(TEXT("name"), Name);
	MCPSetDeleteAssetRollback(Result, Created.Asset->GetPathName());

	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::GetNiagaraInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Err = RequireStringAlt(Params, TEXT("assetPath"), TEXT("path"), AssetPath)) return Err;

	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *AssetPath);
	if (!System)
	{
		return MCPError(FString::Printf(TEXT("NiagaraSystem not found: %s"), *AssetPath));
	}

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("name"), System->GetName());
	Result->SetStringField(TEXT("path"), AssetPath);

	const TArray<FNiagaraEmitterHandle>& EmitterHandles = System->GetEmitterHandles();
	Result->SetNumberField(TEXT("emitterCount"), EmitterHandles.Num());

	TArray<TSharedPtr<FJsonValue>> EmitterArray;
	for (const FNiagaraEmitterHandle& Handle : EmitterHandles)
	{
		TSharedPtr<FJsonObject> EmitterObj = MakeShared<FJsonObject>();
		EmitterObj->SetStringField(TEXT("name"), Handle.GetName().ToString());
		EmitterObj->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
		EmitterArray.Add(MakeShared<FJsonValueObject>(EmitterObj));
	}
	Result->SetArrayField(TEXT("emitters"), EmitterArray);
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::ListEmittersInSystem(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireString(Params, TEXT("systemPath"), SystemPath)) return Err;

	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
	if (!System)
	{
		return MCPError(FString::Printf(TEXT("NiagaraSystem not found: %s"), *SystemPath));
	}

	const TArray<FNiagaraEmitterHandle>& EmitterHandles = System->GetEmitterHandles();
	TArray<TSharedPtr<FJsonValue>> EmitterArray;
	for (const FNiagaraEmitterHandle& Handle : EmitterHandles)
	{
		TSharedPtr<FJsonObject> EmitterObj = MakeShared<FJsonObject>();
		EmitterObj->SetStringField(TEXT("name"), Handle.GetName().ToString());
		EmitterObj->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
		EmitterObj->SetStringField(TEXT("uniqueName"), Handle.GetUniqueInstanceName());
		EmitterArray.Add(MakeShared<FJsonValueObject>(EmitterObj));
	}

	auto Result = MCPSuccess();
	Result->SetArrayField(TEXT("emitters"), EmitterArray);
	Result->SetNumberField(TEXT("count"), EmitterArray.Num());
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::CreateNiagaraEmitter(const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (auto Err = RequireString(Params, TEXT("name"), Name)) return Err;

	FString PackagePath = OptionalString(Params, TEXT("packagePath"), TEXT("/Game/VFX"));
	const FString OnConflict = OptionalString(Params, TEXT("onConflict"), TEXT("skip"));

	UClass* EmitterClass = FindObject<UClass>(nullptr, TEXT("/Script/Niagara.NiagaraEmitter"));
	if (!EmitterClass)
	{
		return MCPError(TEXT("NiagaraEmitter class not found - factory not available"));
	}

	// Build the emitter through UNiagaraEmitterFactoryNew, not a bare NewObject.
	// The factory's empty-emitter path initializes a UNiagaraScriptSource +
	// graph (GraphSource) plus default modules and a sprite renderer - the same
	// asset the "Niagara Emitter" content-browser action produces. A bare
	// NewObject'd emitter has GraphSource==nullptr, "succeeds" here, then
	// crashes the editor the moment it is added to a system. IAssetTools::
	// CreateAsset runs FactoryCreateNew headlessly (no ConfigureProperties UI),
	// and the factory defaults (EmitterToCopy=null) select the empty path.
	UNiagaraEmitterFactoryNew* Factory = NewObject<UNiagaraEmitterFactoryNew>();
	FGCRootScope FactoryRoot(Factory);

	// templatePath was documented and never read, so "create an emitter from
	// this template" silently produced an empty default emitter instead - the
	// caller only found out when the new emitter did none of what the template
	// does. The factory's copy path is the same one the content browser's
	// "create from template" uses.
	TStrongObjectPtr<UNiagaraEmitter> TemplateGuard;
	const FString TemplatePath = OptionalString(Params, TEXT("templatePath"));
	if (TemplatePath.IsEmpty() && OptionalBool(Params, TEXT("inherit"), false))
	{
		return MCPError(TEXT("inherit=true needs a templatePath - there is nothing to inherit from otherwise."));
	}
	if (!TemplatePath.IsEmpty())
	{
		// LoadAssetByPath, not a bare LoadObject: asset(list)/asset(search)
		// return "/Game/VFX/E_Fire" and only "/Game/VFX/E_Fire.E_Fire" loads,
		// so the documented path form failed with a misleading type error.
		UNiagaraEmitter* Template = LoadAssetByPath<UNiagaraEmitter>(TemplatePath);
		if (!Template)
		{
			return MCPError(FString::Printf(
				TEXT("templatePath did not resolve to a NiagaraEmitter: %s"), *TemplatePath));
		}
		// EmitterToCopy is a TWeakObjectPtr and nothing else references a
		// freshly loaded asset. If it goes stale before FactoryCreateNew runs,
		// the factory silently falls into its empty-emitter branch and produces
		// the default asset this parameter exists to avoid.
		TemplateGuard.Reset(Template);
		Factory->EmitterToCopy = Template;
		// Copy rather than inherit: an inherited emitter tracks the template and
		// refuses local edits to inherited modules, which is not what a caller
		// asking for a starting point wants, and is not reversible from here.
		Factory->bUseInheritance = OptionalBool(Params, TEXT("inherit"), false);
	}

	auto Created = MCPCreateAssetIdempotent<UObject>(Name, PackagePath, OnConflict, TEXT("NiagaraEmitter"), EmitterClass, Factory);
	if (Created.EarlyReturn) return Created.EarlyReturn;

	UEditorAssetLibrary::SaveAsset(Created.Asset->GetPathName());

	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("path"), Created.Asset->GetPathName());
	Result->SetStringField(TEXT("name"), Name);
	if (!TemplatePath.IsEmpty())
	{
		Result->SetStringField(TEXT("templatePath"), TemplatePath);
		Result->SetBoolField(TEXT("inherited"), Factory->bUseInheritance);
	}
	MCPSetDeleteAssetRollback(Result, Created.Asset->GetPathName());
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::SpawnNiagaraAtLocation(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireString(Params, TEXT("systemPath"), SystemPath)) return Err;

	UNiagaraSystem* NiagaraSystem = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
	if (!NiagaraSystem)
	{
		return MCPError(FString::Printf(TEXT("NiagaraSystem not found: %s"), *SystemPath));
	}

	REQUIRE_EDITOR_WORLD(World);

	// Location accepts nested {x,y,z} or flat x/y/z params (#70).
	FVector Location = OptionalVec3(Params, TEXT("location"));
	if (Location == FVector::ZeroVector)
	{
		ReadVec3Fields(Params, Location);
	}
	FRotator Rotation = OptionalRotator(Params, TEXT("rotation"));
	if (Rotation == FRotator::ZeroRotator)
	{
		ReadRotatorFields(Params, Rotation);
	}

	// Parse scale
	FVector Scale = FVector::OneVector;
	double ScaleX = 1, ScaleY = 1, ScaleZ = 1;
	if (Params->TryGetNumberField(TEXT("scaleX"), ScaleX) ||
		Params->TryGetNumberField(TEXT("scaleY"), ScaleY) ||
		Params->TryGetNumberField(TEXT("scaleZ"), ScaleZ))
	{
		Scale = FVector(ScaleX, ScaleY, ScaleZ);
	}

	// Default autoDestroy to false so editor spawns persist (#66)
	bool bAutoDestroy = OptionalBool(Params, TEXT("autoDestroy"), false);

	// Idempotency: if a label is provided and an actor with that label already exists, short-circuit
	FString Label = OptionalString(Params, TEXT("label"));
	if (auto ExistingActor = MCPCheckActorLabelExists(World, Label, TEXT("skip"), TEXT("Niagara actor")))
	{
		return ExistingActor;
	}

	UNiagaraComponent* SpawnedComponent = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
		World,
		NiagaraSystem,
		Location,
		Rotation,
		Scale,
		bAutoDestroy
	);

	if (!SpawnedComponent)
	{
		return MCPError(TEXT("Failed to spawn Niagara system at location"));
	}

	// Apply label if provided
	if (!Label.IsEmpty())
	{
		SpawnedComponent->GetOwner()->SetActorLabel(*Label);
	}

	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("systemPath"), SystemPath);
	Result->SetStringField(TEXT("componentName"), SpawnedComponent->GetName());
	if (SpawnedComponent->GetOwner())
	{
		Result->SetStringField(TEXT("actorLabel"), SpawnedComponent->GetOwner()->GetActorLabel());
		Result->SetStringField(TEXT("actorPath"), SpawnedComponent->GetOwner()->GetPathName());
		Result->SetStringField(TEXT("actorName"), SpawnedComponent->GetOwner()->GetName());

		// Rollback: delete_actor
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("actorLabel"), SpawnedComponent->GetOwner()->GetActorLabel());
		MCPSetRollback(Result, TEXT("delete_actor"), Payload);
	}

	TSharedPtr<FJsonObject> LocationObj = MakeShared<FJsonObject>();
	LocationObj->SetNumberField(TEXT("x"), Location.X);
	LocationObj->SetNumberField(TEXT("y"), Location.Y);
	LocationObj->SetNumberField(TEXT("z"), Location.Z);
	Result->SetObjectField(TEXT("location"), LocationObj);

	TSharedPtr<FJsonObject> RotationObj = MakeShared<FJsonObject>();
	RotationObj->SetNumberField(TEXT("pitch"), Rotation.Pitch);
	RotationObj->SetNumberField(TEXT("yaw"), Rotation.Yaw);
	RotationObj->SetNumberField(TEXT("roll"), Rotation.Roll);
	Result->SetObjectField(TEXT("rotation"), RotationObj);

	Result->SetBoolField(TEXT("autoDestroy"), bAutoDestroy);
	return MCPResult(Result);
}

// spawn_niagara_actor -- place a PERSISTENT, labeled ANiagaraActor in the editor
// world (unlike spawn_niagara_at_location, which makes a transient component
// parented to WorldSettings that GC's before an offscreen capture and can't be
// reliably found/reactivated). Assigns the system, sets the label, activates.
// (#537) Params: systemPath, location?, rotation?, label?, activate? (default true).
TSharedPtr<FJsonValue> FNiagaraHandlers::SpawnNiagaraActor(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireString(Params, TEXT("systemPath"), SystemPath)) return Err;

	UNiagaraSystem* NiagaraSystem = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
	if (!NiagaraSystem)
	{
		return MCPError(FString::Printf(TEXT("NiagaraSystem not found: %s"), *SystemPath));
	}

	REQUIRE_EDITOR_WORLD(World);

	FVector Location = OptionalVec3(Params, TEXT("location"));
	if (Location == FVector::ZeroVector) ReadVec3Fields(Params, Location);
	FRotator Rotation = OptionalRotator(Params, TEXT("rotation"));
	if (Rotation == FRotator::ZeroRotator) ReadRotatorFields(Params, Rotation);

	const FString Label = OptionalString(Params, TEXT("label"));
	if (auto ExistingActor = MCPCheckActorLabelExists(World, Label, TEXT("skip"), TEXT("Niagara actor")))
	{
		return ExistingActor;
	}

	const bool bActivate = OptionalBool(Params, TEXT("activate"), true);

	FActorSpawnParameters SpawnParams;
	ANiagaraActor* Actor = World->SpawnActor<ANiagaraActor>(ANiagaraActor::StaticClass(), Location, Rotation, SpawnParams);
	if (!Actor)
	{
		return MCPError(TEXT("Failed to spawn ANiagaraActor"));
	}

	UNiagaraComponent* Comp = Actor->GetNiagaraComponent();
	if (!Comp)
	{
		Actor->Destroy();
		return MCPError(TEXT("Spawned ANiagaraActor has no NiagaraComponent"));
	}

	Comp->SetAsset(NiagaraSystem);
	if (!Label.IsEmpty()) Actor->SetActorLabel(*Label);
	if (bActivate) Comp->Activate(/*bReset=*/true);

	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("systemPath"), SystemPath);
	Result->SetStringField(TEXT("actorLabel"), Actor->GetActorLabel());
	Result->SetStringField(TEXT("actorPath"), Actor->GetPathName());
	Result->SetStringField(TEXT("actorName"), Actor->GetName());
	Result->SetBoolField(TEXT("activated"), bActivate);

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("actorLabel"), Actor->GetActorLabel());
	MCPSetRollback(Result, TEXT("delete_actor"), Payload);
	return MCPResult(Result);
}

// reactivate_niagara -- reset + reactivate the Niagara component on a placed
// actor (e.g. to replay a burst before an offscreen capture). (#537)
// Params: actorLabel.
TSharedPtr<FJsonValue> FNiagaraHandlers::ReactivateNiagara(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (auto Err = RequireStringAlt(Params, TEXT("actorLabel"), TEXT("actorPath"), ActorLabel)) return Err;

	REQUIRE_EDITOR_WORLD(World);

	TSharedPtr<FJsonValue> ActorErr;
	AActor* Actor = MCPResolveActor(World, Params, ActorErr);
	if (!Actor) return ActorErr;
	ActorLabel = Actor->GetActorLabel();
	UNiagaraComponent* Comp = Actor->FindComponentByClass<UNiagaraComponent>();
	if (!Comp)
	{
		return MCPError(FString::Printf(TEXT("No NiagaraComponent on actor: %s"), *ActorLabel));
	}

	Comp->Deactivate();
	Comp->ResetSystem();
	Comp->Activate(/*bReset=*/true);

	auto Result = MCPSuccess();
	MCPSetUpdated(Result);
	Result->SetStringField(TEXT("actorLabel"), ActorLabel);
	Result->SetStringField(TEXT("actorPath"), Actor->GetPathName());
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::SetNiagaraParameter(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (auto Err = RequireStringAlt(Params, TEXT("actorLabel"), TEXT("actorPath"), ActorLabel)) return Err;

	FString ParameterName;
	if (auto Err = RequireString(Params, TEXT("parameterName"), ParameterName)) return Err;

	FString ParameterType = OptionalString(Params, TEXT("parameterType"), TEXT("float"));

	REQUIRE_EDITOR_WORLD(World);

	TSharedPtr<FJsonValue> ActorErr;
	AActor* FoundActor = MCPResolveActor(World, Params, ActorErr);
	if (!FoundActor) return ActorErr;
	ActorLabel = FoundActor->GetActorLabel();

	// Get Niagara component from the actor
	UNiagaraComponent* NiagaraComp = FoundActor->FindComponentByClass<UNiagaraComponent>();
	if (!NiagaraComp)
	{
		return MCPError(FString::Printf(TEXT("No NiagaraComponent found on actor: %s"), *ActorLabel));
	}

	auto Result = MCPSuccess();

	// Set parameter based on type
	FName ParamFName(*ParameterName);
	if (ParameterType == TEXT("float"))
	{
		double Value = 0;
		if (!Params->TryGetNumberField(TEXT("value"), Value))
		{
			return MCPError(TEXT("Missing 'value' parameter for float type"));
		}
		NiagaraComp->SetVariableFloat(ParamFName, (float)Value);
		Result->SetNumberField(TEXT("value"), Value);
	}
	else if (ParameterType == TEXT("vector"))
	{
		double VX = 0, VY = 0, VZ = 0;
		Params->TryGetNumberField(TEXT("valueX"), VX);
		Params->TryGetNumberField(TEXT("valueY"), VY);
		Params->TryGetNumberField(TEXT("valueZ"), VZ);
		FVector VecValue(VX, VY, VZ);
		NiagaraComp->SetVariableVec3(ParamFName, VecValue);

		TSharedPtr<FJsonObject> VecObj = MakeShared<FJsonObject>();
		VecObj->SetNumberField(TEXT("x"), VX);
		VecObj->SetNumberField(TEXT("y"), VY);
		VecObj->SetNumberField(TEXT("z"), VZ);
		Result->SetObjectField(TEXT("value"), VecObj);
	}
	else if (ParameterType == TEXT("bool"))
	{
		bool bValue = OptionalBool(Params, TEXT("value"), false);
		NiagaraComp->SetVariableBool(ParamFName, bValue);
		Result->SetBoolField(TEXT("value"), bValue);
	}
	else if (ParameterType == TEXT("int"))
	{
		double IntValue = OptionalNumber(Params, TEXT("value"), 0.0);
		NiagaraComp->SetVariableInt(ParamFName, (int32)IntValue);
		Result->SetNumberField(TEXT("value"), IntValue);
	}
	else
	{
		return MCPError(FString::Printf(TEXT("Unsupported parameter type: %s (use float, vector, bool, or int)"), *ParameterType));
	}

	MCPSetUpdated(Result);
	Result->SetStringField(TEXT("actorLabel"), ActorLabel);
	Result->SetStringField(TEXT("actorPath"), FoundActor->GetPathName());
	Result->SetStringField(TEXT("parameterName"), ParameterName);
	Result->SetStringField(TEXT("parameterType"), ParameterType);
	// No rollback: runtime niagara parameter overrides are ephemeral; replaying is safe.
	return MCPResult(Result);
}

// An emitter is only safely addable to a system when its target version has a
// GraphSource. A bare NewObject'd UNiagaraEmitter (how create_niagara_emitter
// used to build them) or any asset left in a broken state has
// GraphSource==nullptr; passing one to FNiagaraEditorUtilities::AddEmitterToSystem
// makes Niagara's RebuildEmitterNodes dereference it and hard-crashes the editor
// (ensure GraphSource != nullptr at NiagaraEmitter.cpp, then an access violation).
// Every add-path must gate on this instead of trusting the emitter loaded clean.
static bool EmitterHasGraphSource(UNiagaraEmitter* Emitter, const FGuid& Version)
{
	if (!Emitter) return false;
	const FVersionedNiagaraEmitterData* Data = Version.IsValid()
		? Emitter->GetEmitterData(Version)
		: Emitter->GetLatestEmitterData();
	return Data != nullptr && Data->GraphSource != nullptr;
}

TSharedPtr<FJsonValue> FNiagaraHandlers::AddEmitterToSystem(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireString(Params, TEXT("systemPath"), SystemPath)) return Err;

	FString EmitterPath;
	if (auto Err = RequireString(Params, TEXT("emitterPath"), EmitterPath)) return Err;

	// #223: EditorAssetLibrary::LoadAsset returned None for valid Niagara
	// asset paths (likely a class-resolution mismatch). Use LoadObject<>
	// directly which mirrors what unreal.load_object does in Python.
	auto LoadEither = [](const FString& Path) -> UObject*
	{
		FString WithSuffix = Path;
		if (UObject* Hit = LoadObject<UObject>(nullptr, *WithSuffix)) return Hit;
		const FString BaseName = FPaths::GetBaseFilename(Path);
		WithSuffix = FString::Printf(TEXT("%s.%s"), *Path, *BaseName);
		if (UObject* Hit = LoadObject<UObject>(nullptr, *WithSuffix)) return Hit;
		return UEditorAssetLibrary::LoadAsset(Path);
	};

	UNiagaraSystem* System = Cast<UNiagaraSystem>(LoadEither(SystemPath));
	UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(LoadEither(EmitterPath));

	if (!System)
	{
		return MCPError(FString::Printf(TEXT("NiagaraSystem not found: %s"), *SystemPath));
	}
	if (!Emitter)
	{
		return MCPError(FString::Printf(TEXT("NiagaraEmitter not found: %s"), *EmitterPath));
	}

	// Idempotency: if an emitter with the same source asset is already present, short-circuit
	const FName EmitterFName = Emitter->GetFName();
	for (const FNiagaraEmitterHandle& H : System->GetEmitterHandles())
	{
		if (H.GetInstance().Emitter == Emitter || H.GetName() == EmitterFName)
		{
			auto Existed = MCPSuccess();
			MCPSetExisted(Existed);
			Existed->SetStringField(TEXT("systemPath"), SystemPath);
			Existed->SetStringField(TEXT("emitterPath"), EmitterPath);
			Existed->SetStringField(TEXT("emitterHandleName"), H.GetName().ToString());
			Existed->SetNumberField(TEXT("emitterCount"), System->GetEmitterHandles().Num());
			return MCPResult(Existed);
		}
	}

	// #275: the original implementation called System->AddEmitterHandle
	// directly, which crashed the editor when the parent system had zero
	// emitters (live system instances were not killed before mutating the
	// handle list, the overview graph wasn't rebuilt, and name collisions
	// against the system's outer triggered a checkSlow). Use the canonical
	// NiagaraEditor helper instead - it kills system instances, resolves a
	// unique handle name, calls RebuildEmitterNodes, and synchronizes the
	// system's overview graph. This is the same path the editor uses for
	// "Add Emitter to System" in the Niagara System editor.
	const FGuid EmitterVersion = Emitter->GetExposedVersion().VersionGuid;
	// Guard: a GraphSource-less emitter crashes AddEmitterToSystem. Fail cleanly.
	if (!EmitterHasGraphSource(Emitter, EmitterVersion))
	{
		return MCPError(FString::Printf(
			TEXT("Emitter '%s' has no GraphSource - it was created empty/invalid and cannot be added to a system (adding it would crash the editor). Recreate it with create_niagara_emitter, which now initializes a valid graph."),
			*EmitterPath));
	}
	const FGuid HandleId = FNiagaraEditorUtilities::AddEmitterToSystem(*System, *Emitter, EmitterVersion);
	if (!HandleId.IsValid())
	{
		return MCPError(FString::Printf(TEXT("FNiagaraEditorUtilities::AddEmitterToSystem returned invalid handle for %s"), *EmitterPath));
	}

	// #223: SaveAsset by path resolved a different in-memory instance and
	// dropped the new handle. Save the loaded system object directly.
	System->PostEditChange();
	System->MarkPackageDirty();
	UEditorAssetLibrary::SaveLoadedAsset(System, /*bOnlyIfIsDirty=*/false);

	// Re-find the handle by id so the response reports the actual stored name
	// (AddEmitterToSystem deduplicates / renumbers when there's a collision).
	FName StoredHandleName = NAME_None;
	for (const FNiagaraEmitterHandle& H : System->GetEmitterHandles())
	{
		if (H.GetId() == HandleId)
		{
			StoredHandleName = H.GetName();
			break;
		}
	}

	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("systemPath"), SystemPath);
	Result->SetStringField(TEXT("emitterPath"), EmitterPath);
	Result->SetStringField(TEXT("emitterHandleName"), StoredHandleName.ToString());
	Result->SetStringField(TEXT("emitterHandleId"), HandleId.ToString());
	Result->SetNumberField(TEXT("emitterCount"), System->GetEmitterHandles().Num());
	// No rollback: no paired remove_emitter_from_system handler.
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::SetEmitterProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireStringAlt(Params, TEXT("systemPath"), TEXT("assetPath"), SystemPath)) return Err;

	FString EmitterName = OptionalString(Params, TEXT("emitterName"));

	FString PropName;
	if (auto Err = RequireString(Params, TEXT("propertyName"), PropName)) return Err;

	FString Value;
	if (auto Err = RequireString(Params, TEXT("value"), Value)) return Err;

	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
	if (!System)
	{
		return MCPError(FString::Printf(TEXT("NiagaraSystem not found: %s"), *SystemPath));
	}

	// Find the emitter handle by name (use first if no name specified)
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	int32 TargetIdx = -1;
	if (EmitterName.IsEmpty() && Handles.Num() > 0)
	{
		TargetIdx = 0;
	}
	else
	{
		for (int32 i = 0; i < Handles.Num(); i++)
		{
			if (Handles[i].GetName().ToString() == EmitterName || Handles[i].GetUniqueInstanceName() == EmitterName)
			{
				TargetIdx = i;
				break;
			}
		}
	}

	if (TargetIdx < 0)
	{
		TArray<FString> Names;
		for (const FNiagaraEmitterHandle& H : Handles) Names.Add(H.GetName().ToString());
		return MCPError(FString::Printf(
			TEXT("Emitter '%s' not found. Available: [%s]"), *EmitterName, *FString::Join(Names, TEXT(", "))));
	}

	// Try to set the property via reflection on the emitter handle's emitter data
	FVersionedNiagaraEmitterData* EmitterData = Handles[TargetIdx].GetInstance().GetEmitterData();
	if (!EmitterData)
	{
		return MCPError(TEXT("Could not access emitter data"));
	}

	auto Result = MCPSuccess();

	// Handle common properties
	bool bSet = false;
	if (PropName.Equals(TEXT("enabled"), ESearchCase::IgnoreCase))
	{
		const bool bEnabled = Value.ToBool();
		System->Modify();
		// GetEmitterHandles() is const; the editor mutates via a non-const handle.
		FNiagaraEmitterHandle& MutableHandle = const_cast<FNiagaraEmitterHandle&>(Handles[TargetIdx]);
		MutableHandle.SetIsEnabled(bEnabled, *System, /*bRecompileIfChanged*/ true);
		bSet = true;
		Result->SetBoolField(TEXT("enabled"), bEnabled);
	}

	// Try reflection on the EmitterData's properties
	if (!bSet)
	{
		UStruct* Struct = FVersionedNiagaraEmitterData::StaticStruct();
		FProperty* Prop = Struct->FindPropertyByName(FName(*PropName));
		if (Prop)
		{
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(EmitterData);
			const TCHAR* ImportResult = Prop->ImportText_Direct(*Value, ValuePtr, nullptr, PPF_None);
			bSet = (ImportResult != nullptr);
		}
	}

	if (bSet)
	{
		UEditorAssetLibrary::SaveAsset(System->GetPathName());
	}

	if (bSet) MCPSetUpdated(Result);
	Result->SetStringField(TEXT("systemPath"), SystemPath);
	Result->SetStringField(TEXT("emitterName"), EmitterName);
	Result->SetStringField(TEXT("propertyName"), PropName);
	Result->SetStringField(TEXT("value"), Value);
	Result->SetBoolField(TEXT("success"), bSet);
	// No rollback: emitter reflection writes don't capture a comparable previous value cleanly.
	if (!bSet)
	{
		// List available properties
		TArray<FString> PropNames;
		UStruct* Struct = FVersionedNiagaraEmitterData::StaticStruct();
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			PropNames.Add(It->GetName());
		}
		Result->SetStringField(TEXT("error"), FString::Printf(
			TEXT("Property '%s' not found or could not be set. Available: [%s]"),
			*PropName, *FString::Join(PropNames, TEXT(", "))));
	}
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::GetEmitterInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Err = RequireString(Params, TEXT("assetPath"), AssetPath)) return Err;

	UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(UEditorAssetLibrary::LoadAsset(AssetPath));
	if (!Emitter)
	{
		return MCPError(FString::Printf(TEXT("NiagaraEmitter not found: %s"), *AssetPath));
	}

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("path"), AssetPath);
	Result->SetStringField(TEXT("name"), Emitter->GetName());
	Result->SetStringField(TEXT("class"), Emitter->GetClass()->GetName());

	// Include version data properties (#68)
	// UNiagaraEmitter in UE 5.7 requires a version guid to get emitter data
	// Use the exposed version array to get the latest
	const TArray<FNiagaraAssetVersion>& Versions = Emitter->GetAllAvailableVersions();
	FVersionedNiagaraEmitterData* Data = nullptr;
	if (Versions.Num() > 0)
	{
		Data = Emitter->GetEmitterData(Versions.Last().VersionGuid);
	}
	if (Data)
	{
		// Simulation stages / sim target
		Result->SetStringField(TEXT("simTarget"),
			Data->SimTarget == ENiagaraSimTarget::CPUSim ? TEXT("CPU") : TEXT("GPU"));

		// Renderers
		TArray<TSharedPtr<FJsonValue>> RenderersArray;
		for (UNiagaraRendererProperties* Renderer : Data->GetRenderers())
		{
			if (!Renderer) continue;
			TSharedPtr<FJsonObject> RendObj = MakeShared<FJsonObject>();
			RendObj->SetStringField(TEXT("class"), Renderer->GetClass()->GetName());
			RendObj->SetBoolField(TEXT("enabled"), Renderer->GetIsEnabled());
			RenderersArray.Add(MakeShared<FJsonValueObject>(RendObj));
		}
		Result->SetArrayField(TEXT("renderers"), RenderersArray);
		Result->SetNumberField(TEXT("rendererCount"), RenderersArray.Num());

		// List properties via reflection
		TArray<TSharedPtr<FJsonValue>> PropsArray;
		UStruct* Struct = FVersionedNiagaraEmitterData::StaticStruct();
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
			PropObj->SetStringField(TEXT("name"), It->GetName());
			PropObj->SetStringField(TEXT("type"), It->GetCPPType());
			PropsArray.Add(MakeShared<FJsonValueObject>(PropObj));
		}
		Result->SetArrayField(TEXT("properties"), PropsArray);
	}

	return MCPResult(Result);
}

// ===========================================================================
// v0.7.10 - Niagara depth
// ===========================================================================

namespace
{
	FVersionedNiagaraEmitterData* ResolveEmitter(UNiagaraSystem* System, const FString& EmitterName, int32 EmitterIndex, UNiagaraEmitter*& OutEmitter, FGuid& OutVersion)
	{
		OutEmitter = nullptr;
		const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
		int32 TargetIdx = -1;
		if (!EmitterName.IsEmpty())
		{
			for (int32 i = 0; i < Handles.Num(); ++i)
			{
				if (Handles[i].GetName().ToString().Equals(EmitterName, ESearchCase::IgnoreCase)) { TargetIdx = i; break; }
			}
		}
		else if (EmitterIndex >= 0 && EmitterIndex < Handles.Num())
		{
			TargetIdx = EmitterIndex;
		}
		if (TargetIdx < 0) return nullptr;

		FVersionedNiagaraEmitter VE = Handles[TargetIdx].GetInstance();
		OutEmitter = VE.Emitter;
		OutVersion = VE.Version;
		return VE.GetEmitterData();
	}
}

TSharedPtr<FJsonValue> FNiagaraHandlers::ListEmitterRenderers(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireString(Params, TEXT("systemPath"), SystemPath)) return Err;
	FString EmitterName = OptionalString(Params, TEXT("emitterName"), TEXT(""));
	int32 EmitterIndex = OptionalInt(Params, TEXT("emitterIndex"), 0);

	UNiagaraSystem* System = Cast<UNiagaraSystem>(UEditorAssetLibrary::LoadAsset(SystemPath));
	if (!System) return MCPError(FString::Printf(TEXT("System not found: %s"), *SystemPath));

	UNiagaraEmitter* Emitter = nullptr;
	FGuid Version;
	FVersionedNiagaraEmitterData* Data = ResolveEmitter(System, EmitterName, EmitterIndex, Emitter, Version);
	if (!Data) return MCPError(TEXT("Emitter not resolved"));

	TArray<TSharedPtr<FJsonValue>> RArr;
	int32 Idx = 0;
	for (UNiagaraRendererProperties* R : Data->GetRenderers())
	{
		if (!R) { ++Idx; continue; }
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("index"), Idx++);
		O->SetStringField(TEXT("class"), R->GetClass()->GetName());
		O->SetBoolField(TEXT("enabled"), R->GetIsEnabled());
		RArr.Add(MakeShared<FJsonValueObject>(O));
	}

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	Res->SetStringField(TEXT("systemPath"), SystemPath);
	Res->SetStringField(TEXT("emitter"), Emitter ? Emitter->GetName() : TEXT(""));
	Res->SetArrayField(TEXT("renderers"), RArr);
	Res->SetNumberField(TEXT("rendererCount"), RArr.Num());
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::AddEmitterRenderer(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireString(Params, TEXT("systemPath"), SystemPath)) return Err;
	FString RendererType;
	if (auto Err = RequireString(Params, TEXT("rendererType"), RendererType)) return Err;
	FString EmitterName = OptionalString(Params, TEXT("emitterName"), TEXT(""));
	int32 EmitterIndex = OptionalInt(Params, TEXT("emitterIndex"), 0);

	UNiagaraSystem* System = Cast<UNiagaraSystem>(UEditorAssetLibrary::LoadAsset(SystemPath));
	if (!System) return MCPError(FString::Printf(TEXT("System not found: %s"), *SystemPath));

	UNiagaraEmitter* Emitter = nullptr;
	FGuid Version;
	FVersionedNiagaraEmitterData* Data = ResolveEmitter(System, EmitterName, EmitterIndex, Emitter, Version);
	if (!Data || !Emitter) return MCPError(TEXT("Emitter not resolved"));

	UClass* RendererClass = nullptr;
	if (RendererType.Equals(TEXT("sprite"), ESearchCase::IgnoreCase))        RendererClass = UNiagaraSpriteRendererProperties::StaticClass();
	else if (RendererType.Equals(TEXT("mesh"), ESearchCase::IgnoreCase))     RendererClass = UNiagaraMeshRendererProperties::StaticClass();
	else if (RendererType.Equals(TEXT("ribbon"), ESearchCase::IgnoreCase))   RendererClass = UNiagaraRibbonRendererProperties::StaticClass();
	else
	{
		RendererClass = FindObject<UClass>(nullptr, *RendererType);
		if (!RendererClass) RendererClass = FindClassByShortName(RendererType);
	}
	if (!RendererClass || !RendererClass->IsChildOf(UNiagaraRendererProperties::StaticClass()))
	{
		return MCPError(FString::Printf(TEXT("Unknown renderer type: %s"), *RendererType));
	}

	UNiagaraRendererProperties* NewRenderer = NewObject<UNiagaraRendererProperties>(Emitter, RendererClass, NAME_None, RF_Transactional);
	Emitter->Modify();
	Emitter->AddRenderer(NewRenderer, Version);
	Emitter->PostEditChange();
	UEditorAssetLibrary::SaveLoadedAsset(System);

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	MCPSetCreated(Res);
	Res->SetStringField(TEXT("rendererClass"), RendererClass->GetName());
	Res->SetStringField(TEXT("emitter"), Emitter->GetName());
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::RemoveEmitterRenderer(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireString(Params, TEXT("systemPath"), SystemPath)) return Err;
	const int32 RendererIndex = OptionalInt(Params, TEXT("rendererIndex"), -1);
	FString EmitterName = OptionalString(Params, TEXT("emitterName"), TEXT(""));
	int32 EmitterIndex = OptionalInt(Params, TEXT("emitterIndex"), 0);

	if (RendererIndex < 0) return MCPError(TEXT("Missing 'rendererIndex'"));

	UNiagaraSystem* System = Cast<UNiagaraSystem>(UEditorAssetLibrary::LoadAsset(SystemPath));
	if (!System) return MCPError(FString::Printf(TEXT("System not found: %s"), *SystemPath));

	UNiagaraEmitter* Emitter = nullptr;
	FGuid Version;
	FVersionedNiagaraEmitterData* Data = ResolveEmitter(System, EmitterName, EmitterIndex, Emitter, Version);
	if (!Data || !Emitter) return MCPError(TEXT("Emitter not resolved"));

	TArray<UNiagaraRendererProperties*> Renderers = Data->GetRenderers();
	if (RendererIndex >= Renderers.Num()) return MCPError(TEXT("rendererIndex out of range"));

	UNiagaraRendererProperties* ToRemove = Renderers[RendererIndex];
	Emitter->Modify();
	Emitter->RemoveRenderer(ToRemove, Version);
	Emitter->PostEditChange();
	UEditorAssetLibrary::SaveLoadedAsset(System);

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	MCPSetUpdated(Res);
	Res->SetStringField(TEXT("emitter"), Emitter->GetName());
	Res->SetNumberField(TEXT("removedIndex"), RendererIndex);
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::SetRendererProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireString(Params, TEXT("systemPath"), SystemPath)) return Err;
	FString PropertyName;
	if (auto Err = RequireString(Params, TEXT("propertyName"), PropertyName)) return Err;
	const int32 RendererIndex = OptionalInt(Params, TEXT("rendererIndex"), 0);
	FString EmitterName = OptionalString(Params, TEXT("emitterName"), TEXT(""));
	int32 EmitterIndex = OptionalInt(Params, TEXT("emitterIndex"), 0);

	UNiagaraSystem* System = Cast<UNiagaraSystem>(UEditorAssetLibrary::LoadAsset(SystemPath));
	if (!System) return MCPError(FString::Printf(TEXT("System not found: %s"), *SystemPath));

	UNiagaraEmitter* Emitter = nullptr;
	FGuid Version;
	FVersionedNiagaraEmitterData* Data = ResolveEmitter(System, EmitterName, EmitterIndex, Emitter, Version);
	if (!Data || !Emitter) return MCPError(TEXT("Emitter not resolved"));

	TArray<UNiagaraRendererProperties*> Renderers = Data->GetRenderers();
	if (RendererIndex >= Renderers.Num() || !Renderers[RendererIndex])
	{
		return MCPError(TEXT("rendererIndex out of range or null"));
	}
	UNiagaraRendererProperties* R = Renderers[RendererIndex];

	FProperty* Prop = R->GetClass()->FindPropertyByName(*PropertyName);
	if (!Prop) return MCPError(FString::Printf(TEXT("Property not found: %s"), *PropertyName));

	FString StringValue;
	bool BoolValue = false;
	double NumValue = 0.0;
	R->Modify();
	if (FBoolProperty* BP = CastField<FBoolProperty>(Prop))
	{
		if (!Params->TryGetBoolField(TEXT("value"), BoolValue)) return MCPError(TEXT("Expected bool 'value'"));
		BP->SetPropertyValue(BP->ContainerPtrToValuePtr<void>(R), BoolValue);
	}
	else if (FNumericProperty* NP = CastField<FNumericProperty>(Prop))
	{
		if (!Params->TryGetNumberField(TEXT("value"), NumValue)) return MCPError(TEXT("Expected numeric 'value'"));
		// SetFloatingPointPropertyValue is only implemented for float/double
		// properties; integer and enum-backed numerics need the integer setter.
		void* Addr = NP->ContainerPtrToValuePtr<void>(R);
		if (NP->IsFloatingPoint())
		{
			NP->SetFloatingPointPropertyValue(Addr, NumValue);
		}
		else
		{
			NP->SetIntPropertyValue(Addr, (int64)FMath::RoundToDouble(NumValue));
		}
	}
	else if (FStrProperty* SP = CastField<FStrProperty>(Prop))
	{
		if (!Params->TryGetStringField(TEXT("value"), StringValue)) return MCPError(TEXT("Expected string 'value'"));
		SP->SetPropertyValue(SP->ContainerPtrToValuePtr<void>(R), StringValue);
	}
	else if (FObjectPropertyBase* OP = CastField<FObjectPropertyBase>(Prop))
	{
		// Object/asset reference (e.g. a sprite renderer's Material): 'value' is
		// the asset path.
		if (!Params->TryGetStringField(TEXT("value"), StringValue)) return MCPError(TEXT("Expected string asset path 'value'"));
		UObject* Asset = UEditorAssetLibrary::LoadAsset(StringValue);
		if (!Asset) return MCPError(FString::Printf(TEXT("Asset not found: %s"), *StringValue));
		if (OP->PropertyClass && !Asset->IsA(OP->PropertyClass))
		{
			return MCPError(FString::Printf(TEXT("Asset %s is a %s, not a %s"), *StringValue, *Asset->GetClass()->GetName(), *OP->PropertyClass->GetName()));
		}
		OP->SetObjectPropertyValue(OP->ContainerPtrToValuePtr<void>(R), Asset);
	}
	else
	{
		// #783: every renderer property type that is not bool/number/string/object
		// used to dead-end here, so authoring stalled on the first struct, enum,
		// name or array property. Route the rest through the shared recursive
		// JSON setter (the one behind set_pcg_node_settings and
		// set_component_property) instead of maintaining a type whitelist.
		const TSharedPtr<FJsonValue> RawValue = Params->TryGetField(TEXT("value"));
		if (!RawValue.IsValid())
		{
			return MCPError(TEXT("Expected a 'value' for this property"));
		}
		FString SetError;
		if (!MCPJsonProperty::SetJsonOnProperty(Prop, Prop->ContainerPtrToValuePtr<void>(R), RawValue, SetError))
		{
			return MCPError(FString::Printf(
				TEXT("Could not set %s (%s): %s"), *PropertyName, *Prop->GetCPPType(), *SetError));
		}
	}
	R->PostEditChange();
	Emitter->PostEditChange();
	UEditorAssetLibrary::SaveLoadedAsset(System);

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	MCPSetUpdated(Res);
	Res->SetStringField(TEXT("property"), PropertyName);
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::InspectDataInterface(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireString(Params, TEXT("systemPath"), SystemPath)) return Err;

	UNiagaraSystem* System = Cast<UNiagaraSystem>(UEditorAssetLibrary::LoadAsset(SystemPath));
	if (!System) return MCPError(FString::Printf(TEXT("System not found: %s"), *SystemPath));

	TArray<TSharedPtr<FJsonValue>> DIs;

	const FNiagaraUserRedirectionParameterStore& UserParams = System->GetExposedParameters();
	TArray<FNiagaraVariable> AllVars;
	UserParams.GetParameters(AllVars);
	for (const FNiagaraVariable& Var : AllVars)
	{
		if (!Var.IsDataInterface()) continue;
		UNiagaraDataInterface* DI = UserParams.GetDataInterface(Var);
		if (!DI) continue;
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("name"), Var.GetName().ToString());
		O->SetStringField(TEXT("class"), DI->GetClass()->GetName());
		O->SetStringField(TEXT("scope"), TEXT("user"));
		DIs.Add(MakeShared<FJsonValueObject>(O));
	}

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	Res->SetStringField(TEXT("systemPath"), SystemPath);
	Res->SetArrayField(TEXT("dataInterfaces"), DIs);
	Res->SetNumberField(TEXT("count"), DIs.Num());
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::CreateNiagaraSystemFromSpec(const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (auto Err = RequireString(Params, TEXT("name"), Name)) return Err;
	FString PackagePath = OptionalString(Params, TEXT("packagePath"), TEXT("/Game/VFX"));

	const TArray<TSharedPtr<FJsonValue>>* EmittersArr = nullptr;
	Params->TryGetArrayField(TEXT("emitters"), EmittersArr);

	// Create the system through UNiagaraSystemFactoryNew, not a bare NewObject.
	// A NewObject'd UNiagaraSystem has no system spawn/update scripts and no
	// editor data; FNiagaraEditorUtilities::AddEmitterToSystem then dereferences
	// that missing state and hard-crashes the editor (access violation). The
	// factory's InitializeSystem sets up the scripts + editor data + default
	// nodes - the same asset the plain `create` action produces. (Mirrors
	// CreateNiagaraSystem.)
	UClass* SystemFactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraSystemFactoryNew"));
	UFactory* SystemFactory = SystemFactoryClass
		? Cast<UFactory>(NewObject<UObject>(GetTransientPackage(), SystemFactoryClass))
		: nullptr;
	auto Created = MCPCreateAssetIdempotent<UNiagaraSystem>(Name, PackagePath, OptionalString(Params, TEXT("onConflict"), TEXT("skip")), TEXT("NiagaraSystem"), SystemFactory);
	if (Created.EarlyReturn) return Created.EarlyReturn;
	UNiagaraSystem* System = Created.Asset;

	int32 AddedEmitters = 0;
	TArray<FString> SkippedEmitters;
	if (EmittersArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *EmittersArr)
		{
			const TSharedPtr<FJsonObject>* EmitterObj = nullptr;
			if (!V->TryGetObject(EmitterObj)) continue;
			FString EmitterPath;
			if (!(*EmitterObj)->TryGetStringField(TEXT("path"), EmitterPath)) continue;
			// #223: same load-asset gap as add_emitter_to_system - use
			// LoadObject<> with both bare and Path.Path forms.
			UNiagaraEmitter* Source = LoadObject<UNiagaraEmitter>(nullptr, *EmitterPath);
			if (!Source)
			{
				const FString WithSuffix = FString::Printf(TEXT("%s.%s"), *EmitterPath, *FPaths::GetBaseFilename(EmitterPath));
				Source = LoadObject<UNiagaraEmitter>(nullptr, *WithSuffix);
			}
			if (!Source) Source = Cast<UNiagaraEmitter>(UEditorAssetLibrary::LoadAsset(EmitterPath));
			if (!Source) continue;
			const FGuid Version = Source->GetExposedVersion().VersionGuid;
			// Guard: skip GraphSource-less emitters instead of crashing the
			// editor. Surfaced in the result so the caller knows which ones
			// failed and can recreate them.
			if (!EmitterHasGraphSource(Source, Version))
			{
				SkippedEmitters.Add(EmitterPath);
				continue;
			}
			// #275: route through FNiagaraEditorUtilities so we get
			// KillSystemInstances + RebuildEmitterNodes + unique-name
			// resolution instead of mutating the handle list raw.
			const FGuid HandleId = FNiagaraEditorUtilities::AddEmitterToSystem(*System, *Source, Version);
			if (HandleId.IsValid()) ++AddedEmitters;
		}
	}

	System->PostEditChange();
	UEditorAssetLibrary::SaveLoadedAsset(System);

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	MCPSetCreated(Res);
	Res->SetStringField(TEXT("path"), System->GetPathName());
	Res->SetNumberField(TEXT("emittersAdded"), AddedEmitters);
	if (SkippedEmitters.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Skipped;
		for (const FString& S : SkippedEmitters) Skipped.Add(MakeShared<FJsonValueString>(S));
		Res->SetArrayField(TEXT("skippedEmitters"), Skipped);
		Res->SetStringField(TEXT("warning"), FString::Printf(
			TEXT("%d emitter(s) were skipped because they have no GraphSource (created empty/invalid). Recreate them with create_niagara_emitter."),
			SkippedEmitters.Num()));
	}
	MCPSetDeleteAssetRollback(Res, System->GetPathName());
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::GetCompiledHLSL(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireString(Params, TEXT("systemPath"), SystemPath)) return Err;
	FString EmitterName = OptionalString(Params, TEXT("emitterName"), TEXT(""));
	int32 EmitterIndex = OptionalInt(Params, TEXT("emitterIndex"), 0);

	UNiagaraSystem* System = Cast<UNiagaraSystem>(UEditorAssetLibrary::LoadAsset(SystemPath));
	if (!System) return MCPError(FString::Printf(TEXT("System not found: %s"), *SystemPath));

	UNiagaraEmitter* Emitter = nullptr;
	FGuid Version;
	FVersionedNiagaraEmitterData* Data = ResolveEmitter(System, EmitterName, EmitterIndex, Emitter, Version);
	if (!Data) return MCPError(TEXT("Emitter not resolved"));

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	Res->SetStringField(TEXT("systemPath"), SystemPath);

	if (Data->SimTarget == ENiagaraSimTarget::GPUComputeSim)
	{
		UNiagaraScript* Script = Data->GetGPUComputeScript();
		if (Script)
		{
			Res->SetStringField(TEXT("scriptName"), Script->GetName());
			Res->SetBoolField(TEXT("isCompiled"), Script->IsCompilable());
		}
	}
	else
	{
		Res->SetStringField(TEXT("note"), TEXT("Emitter is CPU-sim; no compiled HLSL available"));
	}
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::ListSystemParameters(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireString(Params, TEXT("systemPath"), SystemPath)) return Err;

	UNiagaraSystem* System = Cast<UNiagaraSystem>(UEditorAssetLibrary::LoadAsset(SystemPath));
	if (!System) return MCPError(FString::Printf(TEXT("System not found: %s"), *SystemPath));

	const FNiagaraUserRedirectionParameterStore& UserParams = System->GetExposedParameters();
	TArray<FNiagaraVariable> Vars;
	UserParams.GetParameters(Vars);

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FNiagaraVariable& V : Vars)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("name"), V.GetName().ToString());
		O->SetStringField(TEXT("type"), V.GetType().GetName());
		O->SetBoolField(TEXT("isDataInterface"), V.IsDataInterface());
		Arr.Add(MakeShared<FJsonValueObject>(O));
	}

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	Res->SetStringField(TEXT("systemPath"), SystemPath);
	Res->SetArrayField(TEXT("parameters"), Arr);
	Res->SetNumberField(TEXT("parameterCount"), Arr.Num());
	return MCPResult(Res);
}

// ===========================================================================
// v0.7.14 - module inputs, static switches, HLSL modules
// ===========================================================================

namespace
{
	struct FScriptSlot
	{
		FString Context;
		UNiagaraScript* Script;
	};

	void CollectEmitterScripts(FVersionedNiagaraEmitterData* Data, const FString& StackContext, TArray<FScriptSlot>& Out)
	{
		const bool bAll = StackContext.IsEmpty() || StackContext.Equals(TEXT("all"), ESearchCase::IgnoreCase);
		if (!Data) return;
		if (bAll || StackContext.Equals(TEXT("ParticleSpawn"), ESearchCase::IgnoreCase))  Out.Add({TEXT("ParticleSpawn"),  Data->SpawnScriptProps.Script});
		if (bAll || StackContext.Equals(TEXT("ParticleUpdate"), ESearchCase::IgnoreCase)) Out.Add({TEXT("ParticleUpdate"), Data->UpdateScriptProps.Script});
		if (bAll || StackContext.Equals(TEXT("EmitterSpawn"), ESearchCase::IgnoreCase))   Out.Add({TEXT("EmitterSpawn"),   Data->EmitterSpawnScriptProps.Script});
		if (bAll || StackContext.Equals(TEXT("EmitterUpdate"), ESearchCase::IgnoreCase))  Out.Add({TEXT("EmitterUpdate"),  Data->EmitterUpdateScriptProps.Script});
	}

	UNiagaraGraph* GraphOfScript(UNiagaraScript* Script)
	{
		if (!Script) return nullptr;
		UNiagaraScriptSource* Src = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
		return Src ? Src->NodeGraph : nullptr;
	}

	TSharedPtr<FJsonObject> NiagaraPinToJson(const UEdGraphPin* Pin)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("name"), Pin->PinName.ToString());
		O->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
		O->SetStringField(TEXT("subCategory"), Pin->PinType.PinSubCategory.ToString());
		O->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
		O->SetBoolField(TEXT("linked"), Pin->LinkedTo.Num() > 0);
		if (Pin->LinkedTo.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Links;
			for (UEdGraphPin* L : Pin->LinkedTo)
			{
				if (L && L->GetOwningNode())
				{
					Links.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("%s.%s"), *L->GetOwningNode()->GetName(), *L->PinName.ToString())));
				}
			}
			O->SetArrayField(TEXT("linkedTo"), Links);
		}
		return O;
	}

	// Shared node-to-JSON for function-call module nodes: class, function name,
	// script asset, enabled state, assignment targets/defaults, custom HLSL,
	// and pins. Used by describe_module_graph and describe_script_graph.
	TSharedPtr<FJsonValue> ModuleNodeToJson(UNiagaraNodeFunctionCall* FC, const FString& StackContext)
	{
		TSharedPtr<FJsonObject> ModObj = MakeShared<FJsonObject>();
		ModObj->SetStringField(TEXT("stackContext"), StackContext);
		ModObj->SetStringField(TEXT("moduleName"), FC->GetFunctionName());
		ModObj->SetStringField(TEXT("nodeClass"), FC->GetClass()->GetName());
		ModObj->SetStringField(TEXT("scriptAsset"), FC->FunctionScript ? FC->FunctionScript->GetPathName() : FString());
		ModObj->SetBoolField(TEXT("enabled"), FC->IsNodeEnabled());

		if (UNiagaraNodeAssignment* Assign = Cast<UNiagaraNodeAssignment>(FC))
		{
			TArray<TSharedPtr<FJsonValue>> Targets;
			for (const FNiagaraVariable& V : Assign->GetAssignmentTargets())
			{
				Targets.Add(MakeShared<FJsonValueString>(V.GetName().ToString()));
			}
			ModObj->SetArrayField(TEXT("assignmentTargets"), Targets);
			const TArray<FString>& Defaults = Assign->GetAssignmentDefaults();
			TArray<TSharedPtr<FJsonValue>> DefArr;
			for (const FString& D : Defaults) DefArr.Add(MakeShared<FJsonValueString>(D));
			ModObj->SetArrayField(TEXT("assignmentDefaults"), DefArr);
		}
		if (UNiagaraNodeCustomHlsl* Hlsl = Cast<UNiagaraNodeCustomHlsl>(FC))
		{
			FString HlslBody;
			if (FStrProperty* HlslProp = CastField<FStrProperty>(
				Hlsl->GetClass()->FindPropertyByName(TEXT("CustomHlsl"))))
			{
				HlslBody = HlslProp->GetPropertyValue_InContainer(Hlsl);
			}
			ModObj->SetStringField(TEXT("hlsl"), HlslBody);
		}

		TArray<TSharedPtr<FJsonValue>> Pins;
		for (UEdGraphPin* Pin : FC->Pins)
		{
			if (!Pin) continue;
			TSharedPtr<FJsonObject> PinObj = PinToJson(Pin);
			PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Output ? TEXT("output") : TEXT("input"));
			Pins.Add(MakeShared<FJsonValueObject>(PinObj));
		}
		ModObj->SetArrayField(TEXT("pins"), Pins);
		return MakeShared<FJsonValueObject>(ModObj);
	}

	// Shared node-to-JSON for NiagaraNodeInput data-interface nodes.
	TSharedPtr<FJsonValue> DIInputNodeToJson(UNiagaraNodeInput* InputNode, const FString& StackContext)
	{
		TSharedPtr<FJsonObject> Dio = MakeShared<FJsonObject>();
		Dio->SetStringField(TEXT("stackContext"), StackContext);
		Dio->SetStringField(TEXT("nodeName"), InputNode->GetName());
		Dio->SetStringField(TEXT("inputName"), InputNode->Input.GetName().ToString());
		UNiagaraDataInterface* DI = nullptr;
		if (FObjectPropertyBase* DIProp = CastField<FObjectPropertyBase>(
			InputNode->GetClass()->FindPropertyByName(TEXT("DataInterface"))))
		{
			DI = Cast<UNiagaraDataInterface>(DIProp->GetObjectPropertyValue_InContainer(InputNode));
		}
		Dio->SetStringField(TEXT("class"), DI ? DI->GetClass()->GetName() : TEXT("(null)"));
		if (UNiagaraDataInterfaceParticleRead* Read = Cast<UNiagaraDataInterfaceParticleRead>(DI))
		{
			Dio->SetStringField(TEXT("bindingMode"),
				Read->EmitterBinding.BindingMode == ENiagaraDataInterfaceEmitterBindingMode::Self
					? TEXT("Self") : TEXT("Other"));
			Dio->SetStringField(TEXT("sourceEmitter"), Read->EmitterBinding.EmitterName.ToString());
		}
		return MakeShared<FJsonValueObject>(Dio);
	}

	ENiagaraScriptUsage UsageOfContext(const FString& Ctx)
	{
		if (Ctx.Equals(TEXT("ParticleUpdate"), ESearchCase::IgnoreCase)) return ENiagaraScriptUsage::ParticleUpdateScript;
		if (Ctx.Equals(TEXT("EmitterSpawn"), ESearchCase::IgnoreCase))   return ENiagaraScriptUsage::EmitterSpawnScript;
		if (Ctx.Equals(TEXT("EmitterUpdate"), ESearchCase::IgnoreCase))  return ENiagaraScriptUsage::EmitterUpdateScript;
		return ENiagaraScriptUsage::ParticleSpawnScript;
	}

	// Parse a string into the raw byte layout of a Niagara input type. Covers the
	// scalar/vector/color types module inputs almost always use. Returns false
	// with a reason for unsupported types.
	bool FillNiagaraValueBytes(const FNiagaraTypeDefinition& T, const FString& Value, TArray<uint8>& Out, FString& OutErr)
	{
		auto ParseFloats = [](const FString& S, int32 N, TArray<float>& F)
		{
			TArray<FString> Parts;
			S.ParseIntoArray(Parts, TEXT(","), true);
			if (Parts.Num() == 1) S.ParseIntoArray(Parts, TEXT(" "), true);
			for (const FString& P : Parts) F.Add(FCString::Atof(*P.TrimStartAndEnd()));
			return F.Num() >= N;
		};

		if (T == FNiagaraTypeDefinition::GetFloatDef())
		{
			float V = FCString::Atof(*Value);
			Out.SetNumUninitialized(sizeof(float)); FMemory::Memcpy(Out.GetData(), &V, sizeof(float)); return true;
		}
		if (T == FNiagaraTypeDefinition::GetIntDef())
		{
			int32 V = FCString::Atoi(*Value);
			Out.SetNumUninitialized(sizeof(int32)); FMemory::Memcpy(Out.GetData(), &V, sizeof(int32)); return true;
		}
		if (T == FNiagaraTypeDefinition::GetBoolDef())
		{
			FNiagaraBool B; B.SetValue(Value.ToBool() || Value == TEXT("1"));
			Out.SetNumUninitialized(sizeof(FNiagaraBool)); FMemory::Memcpy(Out.GetData(), &B, sizeof(FNiagaraBool)); return true;
		}
		if (T == FNiagaraTypeDefinition::GetVec2Def())
		{
			TArray<float> F; if (!ParseFloats(Value, 2, F)) { OutErr = TEXT("expected 2 comma-separated floats"); return false; }
			FVector2f V(F[0], F[1]); Out.SetNumUninitialized(sizeof(V)); FMemory::Memcpy(Out.GetData(), &V, sizeof(V)); return true;
		}
		if (T == FNiagaraTypeDefinition::GetVec3Def())
		{
			TArray<float> F; if (!ParseFloats(Value, 3, F)) { OutErr = TEXT("expected 3 comma-separated floats"); return false; }
			FVector3f V(F[0], F[1], F[2]); Out.SetNumUninitialized(sizeof(V)); FMemory::Memcpy(Out.GetData(), &V, sizeof(V)); return true;
		}
		if (T == FNiagaraTypeDefinition::GetVec4Def() || T == FNiagaraTypeDefinition::GetColorDef())
		{
			TArray<float> F; if (!ParseFloats(Value, 4, F)) { OutErr = TEXT("expected 4 comma-separated floats"); return false; }
			FVector4f V(F[0], F[1], F[2], F[3]); Out.SetNumUninitialized(sizeof(V)); FMemory::Memcpy(Out.GetData(), &V, sizeof(V)); return true;
		}
		OutErr = FString::Printf(TEXT("unsupported input type '%s' for override-map set"), *T.GetName());
		return false;
	}

}


TSharedPtr<FJsonValue> FNiagaraHandlers::ListModuleInputs(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireString(Params, TEXT("systemPath"), SystemPath)) return Err;
	FString EmitterName = OptionalString(Params, TEXT("emitterName"), TEXT(""));
	int32 EmitterIndex = OptionalInt(Params, TEXT("emitterIndex"), 0);
	FString StackContext = OptionalString(Params, TEXT("stackContext"), TEXT("all"));
	FString ModuleFilter = OptionalString(Params, TEXT("moduleName"), TEXT(""));

	UNiagaraSystem* System = Cast<UNiagaraSystem>(UEditorAssetLibrary::LoadAsset(SystemPath));
	if (!System) return MCPError(FString::Printf(TEXT("System not found: %s"), *SystemPath));

	UNiagaraEmitter* Emitter = nullptr;
	FGuid Version;
	FVersionedNiagaraEmitterData* Data = ResolveEmitter(System, EmitterName, EmitterIndex, Emitter, Version);
	if (!Data) return MCPError(TEXT("Emitter not resolved"));

	TArray<FScriptSlot> Scripts;
	CollectEmitterScripts(Data, StackContext, Scripts);

	TArray<TSharedPtr<FJsonValue>> ModulesArr;
	for (const FScriptSlot& Slot : Scripts)
	{
		UNiagaraGraph* Graph = GraphOfScript(Slot.Script);
		if (!Graph) continue;
		for (UEdGraphNode* N : Graph->Nodes)
		{
			UNiagaraNodeFunctionCall* FC = Cast<UNiagaraNodeFunctionCall>(N);
			if (!FC) continue;
			const FString ModName = FC->GetFunctionName();
			if (!ModuleFilter.IsEmpty() && !ModName.Equals(ModuleFilter, ESearchCase::IgnoreCase)) continue;

			TSharedPtr<FJsonObject> ModObj = MakeShared<FJsonObject>();
			ModObj->SetStringField(TEXT("stackContext"), Slot.Context);
			ModObj->SetStringField(TEXT("moduleName"), ModName);
			ModObj->SetStringField(TEXT("scriptAsset"), FC->FunctionScript ? FC->FunctionScript->GetPathName() : FString());

			// #784: the function-call node's PINS are only the compile-time
			// switches and enums. The values an author actually sets - Spawn
			// Rate, Lifetime, Colour, Sprite Size - are module-script inputs
			// held in the override / rapid-iteration map, so enumerating pins
			// listed everything except the things worth setting. Use the same
			// enumeration set_module_input writes through, and read each value
			// back so the list is discoverable AND verifiable.
			TArray<TSharedPtr<FJsonValue>> Inputs;
			{
				FCompileConstantResolver Resolver(
					FVersionedNiagaraEmitter(Emitter, Version), UsageOfContext(Slot.Context));
				TArray<FNiagaraVariable> InputVars;
				FNiagaraStackGraphUtilities::GetStackFunctionInputs(
					*FC, InputVars, Resolver,
					FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);

				TArray<UNiagaraScript*> Dependents;
				for (const FScriptSlot& Dep : Scripts)
				{
					if (Dep.Script) Dependents.Add(Dep.Script);
				}

				for (const FNiagaraVariable& Var : InputVars)
				{
					const FString FullName = Var.GetName().ToString();
					FString Head, Leaf;
					const FString ShortName = FullName.Split(TEXT("."), &Head, &Leaf,
						ESearchCase::IgnoreCase, ESearchDir::FromEnd) ? Leaf : FullName;

					TSharedPtr<FJsonObject> InObj = MakeShared<FJsonObject>();
					InObj->SetStringField(TEXT("name"), ShortName);
					InObj->SetStringField(TEXT("qualifiedName"), FullName);
					// Two inputs can share a leaf name (Module.Position.X and
					// Module.Velocity.X both shorten to "X"), and the leaf is
					// what set_module_input matches on - so flag when the short
					// name is not uniquely addressable.
					{
						int32 SameLeaf = 0;
						for (const FNiagaraVariable& Other : InputVars)
						{
							const FString OtherFull = Other.GetName().ToString();
							FString OH, OL;
							const FString OtherShort = OtherFull.Split(TEXT("."), &OH, &OL,
								ESearchCase::IgnoreCase, ESearchDir::FromEnd) ? OL : OtherFull;
							if (OtherShort == ShortName) ++SameLeaf;
						}
						if (SameLeaf > 1)
						{
							InObj->SetBoolField(TEXT("ambiguousShortName"), true);
							InObj->SetStringField(TEXT("addressAs"), FullName);
						}
					}
					InObj->SetStringField(TEXT("type"), Var.GetType().GetName());

					// Whether set_module_input can bind this input through the
					// override map. FNiagaraStackFunctionInputBinder::GetData is
					// not exported from NiagaraEditor, so the current value
					// cannot be read back from here; the binding check still
					// tells the caller which inputs are writable, and the name
					// is what set_module_input takes.
					FNiagaraStackFunctionInputBinder Binder;
					FText BindErr;
					const bool bBound = Emitter && Binder.TryBind(
						Slot.Script, Dependents, Resolver,
						Emitter->GetUniqueEmitterName(), FC, FName(*ShortName),
						TOptional<FNiagaraTypeDefinition>(Var.GetType()), true, BindErr);
					InObj->SetBoolField(TEXT("settable"), bBound);
					if (!bBound && !BindErr.IsEmpty())
					{
						InObj->SetStringField(TEXT("note"), BindErr.ToString());
					}
					Inputs.Add(MakeShared<FJsonValueObject>(InObj));
				}
			}

			// Compile-time switch/enum pins, kept separate so the two kinds of
			// "input" are not conflated the way they used to be.
			TArray<TSharedPtr<FJsonValue>> SwitchPins;
			TArray<TSharedPtr<FJsonValue>> Outputs;
			for (UEdGraphPin* Pin : FC->Pins)
			{
				if (!Pin) continue;
				if (Pin->Direction == EGPD_Input)  SwitchPins.Add(MakeShared<FJsonValueObject>(NiagaraPinToJson(Pin)));
				if (Pin->Direction == EGPD_Output) Outputs.Add(MakeShared<FJsonValueObject>(NiagaraPinToJson(Pin)));
			}
			ModObj->SetArrayField(TEXT("inputs"), Inputs);
			ModObj->SetNumberField(TEXT("inputCount"), Inputs.Num());
			ModObj->SetArrayField(TEXT("switchPins"), SwitchPins);
			ModObj->SetArrayField(TEXT("outputs"), Outputs);
			ModulesArr.Add(MakeShared<FJsonValueObject>(ModObj));
		}
	}

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	Res->SetStringField(TEXT("systemPath"), SystemPath);
	Res->SetStringField(TEXT("emitter"), Emitter ? Emitter->GetName() : TEXT(""));
	Res->SetArrayField(TEXT("modules"), ModulesArr);
	Res->SetNumberField(TEXT("moduleCount"), ModulesArr.Num());
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::SetModuleInput(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireString(Params, TEXT("systemPath"), SystemPath)) return Err;
	FString ModuleName;
	if (auto Err = RequireString(Params, TEXT("moduleName"), ModuleName)) return Err;
	FString InputName;
	if (auto Err = RequireString(Params, TEXT("inputName"), InputName)) return Err;
	FString Value;
	if (auto Err = RequireString(Params, TEXT("value"), Value)) return Err;
	FString EmitterName = OptionalString(Params, TEXT("emitterName"), TEXT(""));
	int32 EmitterIndex = OptionalInt(Params, TEXT("emitterIndex"), 0);
	FString StackContext = OptionalString(Params, TEXT("stackContext"), TEXT("all"));

	UNiagaraSystem* System = Cast<UNiagaraSystem>(UEditorAssetLibrary::LoadAsset(SystemPath));
	if (!System) return MCPError(FString::Printf(TEXT("System not found: %s"), *SystemPath));

	UNiagaraEmitter* Emitter = nullptr;
	FGuid Version;
	FVersionedNiagaraEmitterData* Data = ResolveEmitter(System, EmitterName, EmitterIndex, Emitter, Version);
	if (!Data) return MCPError(TEXT("Emitter not resolved"));

	TArray<FScriptSlot> Scripts;
	CollectEmitterScripts(Data, StackContext, Scripts);

	int32 SetCount = 0;
	FString PrevValue;
	FString WritePath;
	FString MatchedContext;
	TArray<FString> SeenModules;
	for (const FScriptSlot& Slot : Scripts)
	{
		UNiagaraGraph* Graph = GraphOfScript(Slot.Script);
		if (!Graph) continue;
		for (UEdGraphNode* N : Graph->Nodes)
		{
			UNiagaraNodeFunctionCall* FC = Cast<UNiagaraNodeFunctionCall>(N);
			if (!FC) continue;
			SeenModules.AddUnique(FC->GetFunctionName());
			if (!FC->GetFunctionName().Equals(ModuleName, ESearchCase::IgnoreCase)) continue;

			// Primary path: the real settable module inputs (SpawnRate, Lifetime,
			// sprite size, colour, ...) come from the module SCRIPT, not the
			// function-call node's pins, and their values live in the override /
			// rapid-iteration map. Enumerate the module's inputs, match by name,
			// then set the value through the stack input binder. Falls through to
			// the pin-default path only when this can't bind.
			{
				FCompileConstantResolver Resolver(FVersionedNiagaraEmitter(Emitter, Version), UsageOfContext(Slot.Context));
				TArray<FNiagaraVariable> InputVars;
				FNiagaraStackGraphUtilities::GetStackFunctionInputs(
					*FC, InputVars, Resolver,
					FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);

				auto LeafOf = [](const FString& N)
				{
					FString Head, Leaf;
					return N.Split(TEXT("."), &Head, &Leaf, ESearchCase::IgnoreCase, ESearchDir::FromEnd) ? Leaf : N;
				};
				const FNiagaraVariable* Found = InputVars.FindByPredicate([&](const FNiagaraVariable& V)
				{
					const FString N = V.GetName().ToString();
					return N.Equals(InputName, ESearchCase::IgnoreCase) || LeafOf(N).Equals(InputName, ESearchCase::IgnoreCase);
				});

				if (Found)
				{
					const FString Leaf = LeafOf(Found->GetName().ToString());
					FNiagaraStackFunctionInputBinder Binder;
					TArray<UNiagaraScript*> Dependents;
					for (const FScriptSlot& Dep : Scripts)
					{
						if (Dep.Script) Dependents.Add(Dep.Script);
					}
					FText BindErr;
					if (Binder.TryBind(Slot.Script, Dependents, Resolver, Emitter->GetUniqueEmitterName(), FC,
							FName(*Leaf), TOptional<FNiagaraTypeDefinition>(Found->GetType()), true, BindErr))
					{
						TArray<uint8> Bytes;
						FString VErr;
						if (!FillNiagaraValueBytes(Found->GetType(), Value, Bytes, VErr))
						{
							// The input WAS found; only the value failed to parse.
							// Falling through to the pin-default loop below wrote
							// the unparsed string straight into the pin and still
							// reported success, and reaching the end reported
							// "input not found", which sends the caller after the
							// wrong problem entirely.
							//
							// With stackContext=all an earlier context may already
							// have been written, so returning a bare error here
							// would report failure for a call that did mutate.
							// Say so instead of implying nothing happened.
							return MCPError(FString::Printf(
								TEXT("Cannot use value '%s' for input '%s' (type %s): %s%s"),
								*Value, *InputName, *Found->GetType().GetName(), *VErr,
								SetCount > 0
									? TEXT(" NOTE: this input was already written in an earlier stack context before the failure - the asset is modified and unsaved; re-set it explicitly or discard the change.")
									: TEXT("")));
						}
						{
							FC->Modify();
							Graph->Modify();
							Binder.SetData(Bytes.GetData(), Bytes.Num());
							if (SetCount == 0)
							{
								// The binder's value reader is not exported from
								// NiagaraEditor, so the prior override value cannot be
								// captured here; say so rather than reporting a
								// placeholder that looks like a real value (#769).
								PrevValue = TEXT("(unread: override map)");
								WritePath = TEXT("overrideMap");
							}
							MatchedContext = Slot.Context;
							++SetCount;
							FC->MarkNodeRequiresSynchronization(TEXT("MCP_SetModuleInput"), true);
							Graph->NotifyGraphChanged();
							continue;
						}
					}
				}
			}

			for (UEdGraphPin* Pin : FC->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Input) continue;
				if (Pin->PinName.ToString().Equals(InputName, ESearchCase::IgnoreCase))
				{
					if (SetCount == 0)
					{
						PrevValue = Pin->DefaultValue;
						WritePath = TEXT("pinDefault");
					}
					FC->Modify();
					Graph->Modify();
					Pin->Modify();
					Pin->DefaultValue = Value;
					MatchedContext = Slot.Context;
					++SetCount;
				}
			}
			if (SetCount > 0) FC->MarkNodeRequiresSynchronization(TEXT("MCP_SetModuleInput"), true);
		}
		if (SetCount > 0) Graph->NotifyGraphChanged();
	}

	if (SetCount == 0)
	{
		return MCPError(FString::Printf(TEXT("Module '%s' or input '%s' not found. Modules seen: [%s]"),
			*ModuleName, *InputName, *FString::Join(SeenModules, TEXT(", "))));
	}

	Emitter->PostEditChange();
	System->RequestCompile(false);
	UEditorAssetLibrary::SaveLoadedAsset(System);

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	MCPSetUpdated(Res);
	Res->SetStringField(TEXT("moduleName"), ModuleName);
	Res->SetStringField(TEXT("inputName"), InputName);
	Res->SetStringField(TEXT("value"), Value);
	Res->SetStringField(TEXT("previousValue"), PrevValue);
	Res->SetStringField(TEXT("stackContext"), MatchedContext);
	Res->SetNumberField(TEXT("pinsUpdated"), SetCount);
	Res->SetStringField(TEXT("writePath"), WritePath);
	// #769: this note used to claim override-map inputs would not observe the
	// change, which stopped being true once the binder path landed and led a
	// caller to conclude a write that HAD applied had silently failed.
	Res->SetStringField(TEXT("note"), WritePath == TEXT("overrideMap")
		? TEXT("Written through the stack override map - the same path the Niagara stack editor uses, so the value applies. Reopen the system in the Niagara editor to confirm.")
		: TEXT("Written as a pin default on the function-call node (this input is not override-map bound). Reopen the system in the Niagara editor to confirm."));

	// Rollback: only meaningful when we captured a REAL previous value. On the
	// override-map path previousValue is a placeholder, and emitting a rollback
	// for it would write the literal text "(unread: override map)" into a
	// numeric or colour input.
	if (WritePath == TEXT("overrideMap"))
	{
		return MCPResult(Res);
	}
	TSharedPtr<FJsonObject> RbPayload = MakeShared<FJsonObject>();
	RbPayload->SetStringField(TEXT("systemPath"), SystemPath);
	RbPayload->SetStringField(TEXT("moduleName"), ModuleName);
	RbPayload->SetStringField(TEXT("inputName"), InputName);
	RbPayload->SetStringField(TEXT("value"), PrevValue);
	RbPayload->SetStringField(TEXT("emitterName"), EmitterName);
	RbPayload->SetNumberField(TEXT("emitterIndex"), EmitterIndex);
	RbPayload->SetStringField(TEXT("stackContext"), MatchedContext);
	MCPSetRollback(Res, TEXT("set_niagara_module_input"), RbPayload);
	return MCPResult(Res);
}

namespace
{
	// Map a stack-context string to its script-usage enum and pull the matching
	// UNiagaraScript off the emitter data. Returns false for an unknown context.
	bool ResolveStackTarget(FVersionedNiagaraEmitterData* Data, const FString& Ctx, ENiagaraScriptUsage& OutUsage, UNiagaraScript*& OutScript)
	{
		if (!Data) return false;
		if (Ctx.Equals(TEXT("ParticleSpawn"), ESearchCase::IgnoreCase))  { OutUsage = ENiagaraScriptUsage::ParticleSpawnScript;  OutScript = Data->SpawnScriptProps.Script;         return true; }
		if (Ctx.Equals(TEXT("ParticleUpdate"), ESearchCase::IgnoreCase)) { OutUsage = ENiagaraScriptUsage::ParticleUpdateScript; OutScript = Data->UpdateScriptProps.Script;        return true; }
		if (Ctx.Equals(TEXT("EmitterSpawn"), ESearchCase::IgnoreCase))   { OutUsage = ENiagaraScriptUsage::EmitterSpawnScript;   OutScript = Data->EmitterSpawnScriptProps.Script;  return true; }
		if (Ctx.Equals(TEXT("EmitterUpdate"), ESearchCase::IgnoreCase))  { OutUsage = ENiagaraScriptUsage::EmitterUpdateScript;  OutScript = Data->EmitterUpdateScriptProps.Script; return true; }
		return false;
	}

	// Normalise a module-script reference to a full object path. Accepts a bare
	// package path ("/Niagara/Modules/Emitter/SpawnRate") and appends the
	// ".AssetName" object suffix Niagara scripts require to load.
	FString NormaliseModulePath(const FString& In)
	{
		if (In.Contains(TEXT("."))) return In;
		FString Left, AssetName;
		if (In.Split(TEXT("/"), &Left, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd) && !AssetName.IsEmpty())
		{
			return In + TEXT(".") + AssetName;
		}
		return In;
	}
}

TSharedPtr<FJsonValue> FNiagaraHandlers::AddModule(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireString(Params, TEXT("systemPath"), SystemPath)) return Err;
	FString ModuleScriptRef;
	if (auto Err = RequireString(Params, TEXT("moduleScript"), ModuleScriptRef)) return Err;
	FString StackContext;
	if (auto Err = RequireString(Params, TEXT("stackContext"), StackContext)) return Err;
	FString EmitterName = OptionalString(Params, TEXT("emitterName"), TEXT(""));
	int32 EmitterIndex = OptionalInt(Params, TEXT("emitterIndex"), 0);
	// -1 (default) appends to the end of the stack; >=0 inserts at that index.
	int32 TargetIndex = OptionalInt(Params, TEXT("targetIndex"), -1);

	UNiagaraSystem* System = Cast<UNiagaraSystem>(UEditorAssetLibrary::LoadAsset(SystemPath));
	if (!System) return MCPError(FString::Printf(TEXT("System not found: %s"), *SystemPath));

	UNiagaraEmitter* Emitter = nullptr;
	FGuid Version;
	FVersionedNiagaraEmitterData* Data = ResolveEmitter(System, EmitterName, EmitterIndex, Emitter, Version);
	if (!Data) return MCPError(TEXT("Emitter not resolved"));

	ENiagaraScriptUsage Usage;
	UNiagaraScript* Script = nullptr;
	if (!ResolveStackTarget(Data, StackContext, Usage, Script) || !Script)
	{
		return MCPError(FString::Printf(TEXT("Invalid stackContext '%s'. Use ParticleSpawn|ParticleUpdate|EmitterSpawn|EmitterUpdate."), *StackContext));
	}

	UNiagaraGraph* Graph = GraphOfScript(Script);
	if (!Graph) return MCPError(TEXT("Emitter script has no source graph"));

	// FindEquivalentOutputNode is the NIAGARAEDITOR_API-exported variant
	// (plain FindOutputNode is not exported and won't link).
	UNiagaraNodeOutput* OutputNode = Graph->FindEquivalentOutputNode(Usage);
	if (!OutputNode) return MCPError(FString::Printf(TEXT("No output node for usage in context '%s'"), *StackContext));

	const FString ModulePath = NormaliseModulePath(ModuleScriptRef);
	UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, *ModulePath);
	if (!ModuleScript)
	{
		return MCPError(FString::Printf(TEXT("Module script not found: %s (try a /Niagara/Modules/... path)"), *ModulePath));
	}

	Graph->Modify();
	UNiagaraNodeFunctionCall* NewModule =
		FNiagaraStackGraphUtilities::AddScriptModuleToStack(ModuleScript, *OutputNode, TargetIndex);
	if (!NewModule)
	{
		return MCPError(FString::Printf(TEXT("Failed to add module '%s' to %s stack"), *ModulePath, *StackContext));
	}

	Graph->NotifyGraphChanged();
	if (Emitter) Emitter->PostEditChange();
	System->PostEditChange();
	// Force a compile so the emitter is immediately usable (e.g. a verify step
	// that spawns it and reads particle count).
	System->RequestCompile(false);
	UEditorAssetLibrary::SaveLoadedAsset(System);

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	MCPSetCreated(Res);
	Res->SetStringField(TEXT("systemPath"), SystemPath);
	Res->SetStringField(TEXT("emitter"), Emitter ? Emitter->GetName() : TEXT(""));
	Res->SetStringField(TEXT("stackContext"), StackContext);
	Res->SetStringField(TEXT("moduleScript"), ModulePath);
	Res->SetStringField(TEXT("moduleName"), NewModule->GetFunctionName());
	Res->SetNumberField(TEXT("targetIndex"), TargetIndex);
	Res->SetStringField(TEXT("note"), TEXT("Module node added and wired into the parameter map. Set its inputs with set_niagara_module_input."));
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::RemoveEmitterFromSystem(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireString(Params, TEXT("systemPath"), SystemPath)) return Err;
	FString EmitterName = OptionalString(Params, TEXT("emitterName"), TEXT(""));
	int32 EmitterIndex = OptionalInt(Params, TEXT("emitterIndex"), -1);

	UNiagaraSystem* System = Cast<UNiagaraSystem>(UEditorAssetLibrary::LoadAsset(SystemPath));
	if (!System) return MCPError(FString::Printf(TEXT("System not found: %s"), *SystemPath));

	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	int32 TargetIdx = -1;
	if (!EmitterName.IsEmpty())
	{
		for (int32 i = 0; i < Handles.Num(); ++i)
		{
			if (Handles[i].GetName().ToString().Equals(EmitterName, ESearchCase::IgnoreCase)) { TargetIdx = i; break; }
		}
	}
	else if (EmitterIndex >= 0 && EmitterIndex < Handles.Num())
	{
		TargetIdx = EmitterIndex;
	}
	if (TargetIdx < 0)
	{
		return MCPError(FString::Printf(TEXT("Emitter not found (name='%s', index=%d) in %s"), *EmitterName, EmitterIndex, *SystemPath));
	}

	const FString RemovedName = Handles[TargetIdx].GetName().ToString();
	const FGuid RemovedId = Handles[TargetIdx].GetId();

	System->Modify();
	System->RemoveEmitterHandlesById({ RemovedId });
	System->RequestCompile(false);
	System->PostEditChange();
	UEditorAssetLibrary::SaveLoadedAsset(System);

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	MCPSetUpdated(Res);
	Res->SetStringField(TEXT("systemPath"), SystemPath);
	Res->SetStringField(TEXT("removedEmitter"), RemovedName);
	Res->SetNumberField(TEXT("remainingEmitters"), System->GetEmitterHandles().Num());
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::ValidateSystem(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireString(Params, TEXT("systemPath"), SystemPath)) return Err;

	UNiagaraSystem* System = Cast<UNiagaraSystem>(UEditorAssetLibrary::LoadAsset(SystemPath));
	if (!System) return MCPError(FString::Printf(TEXT("System not found: %s"), *SystemPath));

	TArray<TSharedPtr<FJsonValue>> EArr;
	int32 ValidEmitters = 0;
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	for (const FNiagaraEmitterHandle& H : Handles)
	{
		const bool Enabled = H.GetIsEnabled();
		FVersionedNiagaraEmitter VE = H.GetInstance();
		FVersionedNiagaraEmitterData* Data = VE.GetEmitterData();

		int32 RendererCount = 0;
		if (Data)
		{
			for (UNiagaraRendererProperties* R : Data->GetRenderers())
			{
				if (R && R->GetIsEnabled()) ++RendererCount;
			}
		}

		// A spawn module (SpawnRate / SpawnBurst / SpawnPerUnit) in EmitterUpdate
		// is what makes an emitter emit anything at all.
		bool HasSpawn = false;
		if (Data)
		{
			TArray<FScriptSlot> Scripts;
			CollectEmitterScripts(Data, TEXT("EmitterUpdate"), Scripts);
			for (const FScriptSlot& S : Scripts)
			{
				UNiagaraGraph* G = GraphOfScript(S.Script);
				if (!G) continue;
				for (UEdGraphNode* Nn : G->Nodes)
				{
					UNiagaraNodeFunctionCall* FC = Cast<UNiagaraNodeFunctionCall>(Nn);
					if (FC && FC->GetFunctionName().Contains(TEXT("Spawn"))) { HasSpawn = true; break; }
				}
				if (HasSpawn) break;
			}
		}

		const bool EValid = Enabled && HasSpawn && RendererCount > 0;
		if (EValid) ++ValidEmitters;

		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("name"), H.GetName().ToString());
		O->SetBoolField(TEXT("enabled"), Enabled);
		O->SetBoolField(TEXT("hasSpawnModule"), HasSpawn);
		O->SetNumberField(TEXT("enabledRenderers"), RendererCount);
		O->SetBoolField(TEXT("valid"), EValid);
		EArr.Add(MakeShared<FJsonValueObject>(O));
	}

	const bool Valid = ValidEmitters > 0;
	TSharedPtr<FJsonObject> Res = MCPSuccess();
	Res->SetStringField(TEXT("systemPath"), SystemPath);
	Res->SetBoolField(TEXT("valid"), Valid);
	Res->SetNumberField(TEXT("emitterCount"), Handles.Num());
	Res->SetNumberField(TEXT("validEmitters"), ValidEmitters);
	Res->SetArrayField(TEXT("emitters"), EArr);
	if (!Valid)
	{
		Res->SetStringField(TEXT("reason"), TEXT("No enabled emitter has both a spawn module and an enabled renderer - this system will emit nothing."));
	}
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::ListStaticSwitches(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireString(Params, TEXT("systemPath"), SystemPath)) return Err;
	FString ModuleFilter = OptionalString(Params, TEXT("moduleName"), TEXT(""));
	FString EmitterName = OptionalString(Params, TEXT("emitterName"), TEXT(""));
	int32 EmitterIndex = OptionalInt(Params, TEXT("emitterIndex"), 0);
	FString StackContext = OptionalString(Params, TEXT("stackContext"), TEXT("all"));

	UNiagaraSystem* System = Cast<UNiagaraSystem>(UEditorAssetLibrary::LoadAsset(SystemPath));
	if (!System) return MCPError(FString::Printf(TEXT("System not found: %s"), *SystemPath));

	UNiagaraEmitter* Emitter = nullptr;
	FGuid Version;
	FVersionedNiagaraEmitterData* Data = ResolveEmitter(System, EmitterName, EmitterIndex, Emitter, Version);
	if (!Data) return MCPError(TEXT("Emitter not resolved"));

	TArray<FScriptSlot> Scripts;
	CollectEmitterScripts(Data, StackContext, Scripts);

	TArray<TSharedPtr<FJsonValue>> ModulesArr;
	for (const FScriptSlot& Slot : Scripts)
	{
		UNiagaraGraph* Graph = GraphOfScript(Slot.Script);
		if (!Graph) continue;
		for (UEdGraphNode* N : Graph->Nodes)
		{
			UNiagaraNodeFunctionCall* FC = Cast<UNiagaraNodeFunctionCall>(N);
			if (!FC) continue;
			const FString ModName = FC->GetFunctionName();
			if (!ModuleFilter.IsEmpty() && !ModName.Equals(ModuleFilter, ESearchCase::IgnoreCase)) continue;

			TArray<TSharedPtr<FJsonValue>> Switches;
			UNiagaraGraph* FuncGraph = FC->GetCalledGraph();
			if (FuncGraph)
			{
				const TArray<FNiagaraVariable> SwitchVars = FuncGraph->FindStaticSwitchInputs(false);
				for (const FNiagaraVariable& Var : SwitchVars)
				{
					const FName VarName = Var.GetName();
					UEdGraphPin* SwitchPin = nullptr;
					for (UEdGraphPin* Pin : FC->Pins)
					{
						if (Pin && Pin->Direction == EGPD_Input && Pin->PinName == VarName) { SwitchPin = Pin; break; }
					}
					TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
					O->SetStringField(TEXT("name"), VarName.ToString());
					O->SetStringField(TEXT("type"), Var.GetType().GetName());
					O->SetStringField(TEXT("defaultValue"), SwitchPin ? SwitchPin->DefaultValue : FString());
					O->SetBoolField(TEXT("boundToPin"), SwitchPin != nullptr);
					Switches.Add(MakeShared<FJsonValueObject>(O));
				}
			}
			if (Switches.Num() == 0) continue;
			TSharedPtr<FJsonObject> ModObj = MakeShared<FJsonObject>();
			ModObj->SetStringField(TEXT("stackContext"), Slot.Context);
			ModObj->SetStringField(TEXT("moduleName"), ModName);
			ModObj->SetArrayField(TEXT("staticSwitches"), Switches);
			ModulesArr.Add(MakeShared<FJsonValueObject>(ModObj));
		}
	}

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	Res->SetStringField(TEXT("systemPath"), SystemPath);
	Res->SetStringField(TEXT("emitter"), Emitter ? Emitter->GetName() : TEXT(""));
	Res->SetArrayField(TEXT("modules"), ModulesArr);
	Res->SetNumberField(TEXT("moduleCount"), ModulesArr.Num());
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::SetStaticSwitch(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireString(Params, TEXT("systemPath"), SystemPath)) return Err;
	FString ModuleName;
	if (auto Err = RequireString(Params, TEXT("moduleName"), ModuleName)) return Err;
	FString SwitchName;
	if (auto Err = RequireString(Params, TEXT("switchName"), SwitchName)) return Err;
	FString Value;
	if (auto Err = RequireString(Params, TEXT("value"), Value)) return Err;
	FString EmitterName = OptionalString(Params, TEXT("emitterName"), TEXT(""));
	int32 EmitterIndex = OptionalInt(Params, TEXT("emitterIndex"), 0);
	FString StackContext = OptionalString(Params, TEXT("stackContext"), TEXT("all"));

	UNiagaraSystem* System = Cast<UNiagaraSystem>(UEditorAssetLibrary::LoadAsset(SystemPath));
	if (!System) return MCPError(FString::Printf(TEXT("System not found: %s"), *SystemPath));

	UNiagaraEmitter* Emitter = nullptr;
	FGuid Version;
	FVersionedNiagaraEmitterData* Data = ResolveEmitter(System, EmitterName, EmitterIndex, Emitter, Version);
	if (!Data) return MCPError(TEXT("Emitter not resolved"));

	TArray<FScriptSlot> Scripts;
	CollectEmitterScripts(Data, StackContext, Scripts);

	int32 SetCount = 0;
	FString PrevValue;
	FString MatchedContext;
	for (const FScriptSlot& Slot : Scripts)
	{
		UNiagaraGraph* Graph = GraphOfScript(Slot.Script);
		if (!Graph) continue;
		for (UEdGraphNode* N : Graph->Nodes)
		{
			UNiagaraNodeFunctionCall* FC = Cast<UNiagaraNodeFunctionCall>(N);
			if (!FC) continue;
			if (!FC->GetFunctionName().Equals(ModuleName, ESearchCase::IgnoreCase)) continue;
			// Find the static switch pin by name (FindStaticSwitchInputPin isn't exported, so walk pins).
			UEdGraphPin* SwitchPin = nullptr;
			const FName Needle(*SwitchName);
			for (UEdGraphPin* Pin : FC->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input && Pin->PinName == Needle) { SwitchPin = Pin; break; }
			}
			if (!SwitchPin) continue;
			if (SetCount == 0) PrevValue = SwitchPin->DefaultValue;
			FC->Modify();
			Graph->Modify();
			SwitchPin->Modify();
			SwitchPin->DefaultValue = Value;
			FC->MarkNodeRequiresSynchronization(TEXT("MCP_SetStaticSwitch"), true);
			MatchedContext = Slot.Context;
			++SetCount;
		}
		if (SetCount > 0) Graph->NotifyGraphChanged();
	}

	if (SetCount == 0)
	{
		return MCPError(FString::Printf(TEXT("Static switch '%s' on module '%s' not found"), *SwitchName, *ModuleName));
	}

	Emitter->PostEditChange();
	UEditorAssetLibrary::SaveLoadedAsset(System);

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	MCPSetUpdated(Res);
	Res->SetStringField(TEXT("moduleName"), ModuleName);
	Res->SetStringField(TEXT("switchName"), SwitchName);
	Res->SetStringField(TEXT("value"), Value);
	Res->SetStringField(TEXT("previousValue"), PrevValue);
	Res->SetStringField(TEXT("stackContext"), MatchedContext);
	Res->SetNumberField(TEXT("pinsUpdated"), SetCount);

	TSharedPtr<FJsonObject> RbPayload = MakeShared<FJsonObject>();
	RbPayload->SetStringField(TEXT("systemPath"), SystemPath);
	RbPayload->SetStringField(TEXT("moduleName"), ModuleName);
	RbPayload->SetStringField(TEXT("switchName"), SwitchName);
	RbPayload->SetStringField(TEXT("value"), PrevValue);
	RbPayload->SetStringField(TEXT("emitterName"), EmitterName);
	RbPayload->SetNumberField(TEXT("emitterIndex"), EmitterIndex);
	RbPayload->SetStringField(TEXT("stackContext"), MatchedContext);
	MCPSetRollback(Res, TEXT("set_niagara_static_switch"), RbPayload);
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::CreateModuleFromHlsl(const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (auto Err = RequireString(Params, TEXT("name"), Name)) return Err;
	FString Hlsl;
	if (auto Err = RequireString(Params, TEXT("hlsl"), Hlsl)) return Err;
	FString PackagePath = OptionalString(Params, TEXT("packagePath"), TEXT("/Game/VFX/Modules"));

	// Use the stock module factory to create a baseline module with Param-map get/set scaffolding,
	// then add a CustomHLSL node that carries the user's HLSL body.
	UFactory* Factory = CreateNiagaraEditorFactoryByClassPath(TEXT("/Script/NiagaraEditor.NiagaraModuleScriptFactory"));
	if (!Factory) return MCPError(TEXT("Failed to create Niagara module factory. Ensure Niagara editor module is available."));
	auto Created = MCPCreateAssetIdempotent<UNiagaraScript>(Name, PackagePath, OptionalString(Params, TEXT("onConflict"), TEXT("skip")), TEXT("NiagaraScript"), Factory);
	if (Created.EarlyReturn) return Created.EarlyReturn;
	UNiagaraScript* Script = Created.Asset;

	UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
	UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;
	if (!Graph) return MCPError(TEXT("New module has no graph"));

	// Inject a CustomHLSL node next to the existing Output node.
	FGraphNodeCreator<UNiagaraNodeCustomHlsl> Creator(*Graph);
	UNiagaraNodeCustomHlsl* Custom = Creator.CreateNode();
	Creator.Finalize();
	// SetCustomHlsl / RebuildSignatureFromPins aren't exported from NiagaraEditor.
	// Write the CustomHlsl UPROPERTY directly; Niagara's PostEditChange + ReconstructNode
	// re-parses the HLSL body and regenerates pins.
	if (FProperty* HlslProp = Custom->GetClass()->FindPropertyByName(TEXT("CustomHlsl")))
	{
		if (FStrProperty* SP = CastField<FStrProperty>(HlslProp))
		{
			Custom->Modify();
			SP->SetPropertyValue(SP->ContainerPtrToValuePtr<void>(Custom), Hlsl);
		}
	}
	Custom->ReconstructNode();
	Custom->PostEditChange();

	// Touch inputs/outputs array for informational echo (the CustomHLSL node manages its own pins via HLSL parsing)
	const TArray<TSharedPtr<FJsonValue>>* InputsArr = nullptr;
	Params->TryGetArrayField(TEXT("inputs"), InputsArr);
	const TArray<TSharedPtr<FJsonValue>>* OutputsArr = nullptr;
	Params->TryGetArrayField(TEXT("outputs"), OutputsArr);

	Graph->NotifyGraphChanged();
	Script->MarkPackageDirty();
	UEditorAssetLibrary::SaveLoadedAsset(Script);

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	MCPSetCreated(Res);
	Res->SetStringField(TEXT("path"), Script->GetPathName());
	Res->SetStringField(TEXT("name"), Name);
	Res->SetNumberField(TEXT("hlslLength"), Hlsl.Len());
	Res->SetNumberField(TEXT("requestedInputs"), InputsArr ? InputsArr->Num() : 0);
	Res->SetNumberField(TEXT("requestedOutputs"), OutputsArr ? OutputsArr->Num() : 0);
	Res->SetStringField(TEXT("note"), TEXT("Module scaffold created with embedded CustomHLSL node. Pins are auto-derived from the HLSL body - open the asset to confirm signatures."));
	MCPSetDeleteAssetRollback(Res, Script->GetPathName());
	return MCPResult(Res);
}

// ===========================================================================
// #185 - Create an empty scratch-pad-style Niagara module (NiagaraScript asset)
// ===========================================================================
TSharedPtr<FJsonValue> FNiagaraHandlers::CreateScratchModule(const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (auto Err = RequireString(Params, TEXT("name"), Name)) return Err;
	FString PackagePath = OptionalString(Params, TEXT("packagePath"), TEXT("/Game/VFX"));

	// Use the stock Niagara module factory to create a baseline module script
	UFactory* Factory = CreateNiagaraEditorFactoryByClassPath(TEXT("/Script/NiagaraEditor.NiagaraModuleScriptFactory"));
	if (!Factory) return MCPError(TEXT("Failed to create Niagara module factory. Ensure Niagara editor module is available."));
	auto Created = MCPCreateAssetIdempotent<UNiagaraScript>(Name, PackagePath, OptionalString(Params, TEXT("onConflict"), TEXT("skip")), TEXT("NiagaraScript"), Factory);
	if (Created.EarlyReturn) return Created.EarlyReturn;
	UNiagaraScript* Script = Created.Asset;

	UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
	UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;

	// Optionally add input/output pins on a CustomHLSL node stub so the module has declared parameters
	const TArray<TSharedPtr<FJsonValue>>* InputsArr = nullptr;
	Params->TryGetArrayField(TEXT("inputs"), InputsArr);
	const TArray<TSharedPtr<FJsonValue>>* OutputsArr = nullptr;
	Params->TryGetArrayField(TEXT("outputs"), OutputsArr);

	int32 InputCount = InputsArr ? InputsArr->Num() : 0;
	int32 OutputCount = OutputsArr ? OutputsArr->Num() : 0;

	// If inputs/outputs were requested, inject a CustomHLSL node with a trivial pass-through body
	// that declares the requested parameters so they appear in the module's stack overview.
	if (Graph && (InputCount > 0 || OutputCount > 0))
	{
		// Build a simple HLSL body that declares inputs and maps them to outputs
		FString HlslBody;
		if (InputsArr)
		{
			for (const TSharedPtr<FJsonValue>& V : *InputsArr)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (!V->TryGetObject(Obj)) continue;
				FString PinName, PinType;
				(*Obj)->TryGetStringField(TEXT("name"), PinName);
				(*Obj)->TryGetStringField(TEXT("type"), PinType);
				if (PinType.IsEmpty()) PinType = TEXT("float");
				// Declare as HLSL input: e.g. "float MyInput;"
				HlslBody += FString::Printf(TEXT("%s %s;\n"), *PinType, *PinName);
			}
		}
		if (OutputsArr)
		{
			for (const TSharedPtr<FJsonValue>& V : *OutputsArr)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (!V->TryGetObject(Obj)) continue;
				FString PinName, PinType;
				(*Obj)->TryGetStringField(TEXT("name"), PinName);
				(*Obj)->TryGetStringField(TEXT("type"), PinType);
				if (PinType.IsEmpty()) PinType = TEXT("float");
				HlslBody += FString::Printf(TEXT("out %s %s;\n"), *PinType, *PinName);
			}
		}
		if (HlslBody.IsEmpty())
		{
			HlslBody = TEXT("// Empty scratch module\n");
		}

		FGraphNodeCreator<UNiagaraNodeCustomHlsl> Creator(*Graph);
		UNiagaraNodeCustomHlsl* Custom = Creator.CreateNode();
		Creator.Finalize();

		if (FProperty* HlslProp = Custom->GetClass()->FindPropertyByName(TEXT("CustomHlsl")))
		{
			if (FStrProperty* SP = CastField<FStrProperty>(HlslProp))
			{
				Custom->Modify();
				SP->SetPropertyValue(SP->ContainerPtrToValuePtr<void>(Custom), HlslBody);
			}
		}
		Custom->ReconstructNode();
		Custom->PostEditChange();
		Graph->NotifyGraphChanged();
	}

	Script->MarkPackageDirty();
	UEditorAssetLibrary::SaveLoadedAsset(Script);

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	MCPSetCreated(Res);
	Res->SetStringField(TEXT("path"), Script->GetPathName());
	Res->SetStringField(TEXT("name"), Name);
	Res->SetNumberField(TEXT("requestedInputs"), InputCount);
	Res->SetNumberField(TEXT("requestedOutputs"), OutputCount);
	Res->SetStringField(TEXT("note"), TEXT("Empty scratch module created. Open in Niagara editor to add logic, or use set_niagara_module_input to configure."));
	MCPSetDeleteAssetRollback(Res, Script->GetPathName());
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::DescribeModuleGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireString(Params, TEXT("systemPath"), SystemPath)) return Err;
	FString EmitterName = OptionalString(Params, TEXT("emitterName"), TEXT(""));
	int32 EmitterIndex = OptionalInt(Params, TEXT("emitterIndex"), 0);
	FString StackContext = OptionalString(Params, TEXT("stackContext"), TEXT("all"));
	FString ModuleFilter = OptionalString(Params, TEXT("moduleName"), TEXT(""));

	UNiagaraSystem* System = Cast<UNiagaraSystem>(UEditorAssetLibrary::LoadAsset(SystemPath));
	if (!System) return MCPError(FString::Printf(TEXT("System not found: %s"), *SystemPath));

	UNiagaraEmitter* Emitter = nullptr;
	FGuid Version;
	FVersionedNiagaraEmitterData* Data = ResolveEmitter(System, EmitterName, EmitterIndex, Emitter, Version);
	if (!Data) return MCPError(TEXT("Emitter not resolved"));

	TArray<FScriptSlot> Scripts;
	CollectEmitterScripts(Data, StackContext, Scripts);

	TArray<TSharedPtr<FJsonValue>> ModulesArr;
	TArray<TSharedPtr<FJsonValue>> DataInterfaceNodes;
	for (const FScriptSlot& Slot : Scripts)
	{
		UNiagaraGraph* Graph = GraphOfScript(Slot.Script);
		if (!Graph) continue;

		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(N))
			{
				// GetDataInterface() is not exported from NiagaraEditor; read the
				// private UPROPERTY through reflection instead (same trick the
				// scratch-module writer uses for CustomHlsl).
				UNiagaraDataInterface* DI = nullptr;
				if (FObjectPropertyBase* DIProp = CastField<FObjectPropertyBase>(
					InputNode->GetClass()->FindPropertyByName(TEXT("DataInterface"))))
				{
					DI = Cast<UNiagaraDataInterface>(DIProp->GetObjectPropertyValue_InContainer(InputNode));
				}
				if (!DI) continue;
				TSharedPtr<FJsonObject> Dio = MakeShared<FJsonObject>();
				Dio->SetStringField(TEXT("stackContext"), Slot.Context);
				Dio->SetStringField(TEXT("inputName"), InputNode->Input.GetName().ToString());
				Dio->SetStringField(TEXT("class"), DI->GetClass()->GetName());
				if (UNiagaraDataInterfaceParticleRead* Read = Cast<UNiagaraDataInterfaceParticleRead>(DI))
				{
					Dio->SetStringField(TEXT("bindingMode"),
						Read->EmitterBinding.BindingMode == ENiagaraDataInterfaceEmitterBindingMode::Self
							? TEXT("Self") : TEXT("Other"));
					Dio->SetStringField(TEXT("sourceEmitter"), Read->EmitterBinding.EmitterName.ToString());
				}
				DataInterfaceNodes.Add(MakeShared<FJsonValueObject>(Dio));
				continue;
			}

			UNiagaraNodeFunctionCall* FC = Cast<UNiagaraNodeFunctionCall>(N);
			if (!FC) continue;
			const FString ModName = FC->GetFunctionName();
			if (!ModuleFilter.IsEmpty() && !ModName.Contains(ModuleFilter, ESearchCase::IgnoreCase)) continue;

			TSharedPtr<FJsonObject> ModObj = MakeShared<FJsonObject>();
			ModObj->SetStringField(TEXT("stackContext"), Slot.Context);
			ModObj->SetStringField(TEXT("moduleName"), ModName);
			ModObj->SetStringField(TEXT("nodeClass"), N->GetClass()->GetName());
			ModObj->SetStringField(TEXT("scriptAsset"), FC->FunctionScript ? FC->FunctionScript->GetPathName() : FString());
			ModObj->SetBoolField(TEXT("enabled"), N->IsNodeEnabled());

			if (UNiagaraNodeAssignment* Assign = Cast<UNiagaraNodeAssignment>(FC))
			{
				TArray<TSharedPtr<FJsonValue>> Targets;
				for (const FNiagaraVariable& V : Assign->GetAssignmentTargets())
				{
					Targets.Add(MakeShared<FJsonValueString>(V.GetName().ToString()));
				}
				ModObj->SetArrayField(TEXT("assignmentTargets"), Targets);
				const TArray<FString>& Defaults = Assign->GetAssignmentDefaults();
				TArray<TSharedPtr<FJsonValue>> DefArr;
				for (const FString& D : Defaults) DefArr.Add(MakeShared<FJsonValueString>(D));
				ModObj->SetArrayField(TEXT("assignmentDefaults"), DefArr);
			}
			if (UNiagaraNodeCustomHlsl* Hlsl = Cast<UNiagaraNodeCustomHlsl>(FC))
			{
				// GetCustomHlsl() is not exported; read the UPROPERTY directly.
				FString HlslBody;
				if (FStrProperty* HlslProp = CastField<FStrProperty>(
					Hlsl->GetClass()->FindPropertyByName(TEXT("CustomHlsl"))))
				{
					HlslBody = HlslProp->GetPropertyValue_InContainer(Hlsl);
				}
				ModObj->SetStringField(TEXT("hlsl"), HlslBody);
			}

			TArray<TSharedPtr<FJsonValue>> Pins;
			for (UEdGraphPin* Pin : N->Pins)
			{
				if (!Pin) continue;
				TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
				PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
				PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Output ? TEXT("output") : TEXT("input"));
				PinObj->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
				PinObj->SetBoolField(TEXT("linked"), Pin->LinkedTo.Num() > 0);
				if (Pin->LinkedTo.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> Links;
					for (UEdGraphPin* L : Pin->LinkedTo)
					{
						if (L && L->GetOwningNode())
						{
							Links.Add(MakeShared<FJsonValueString>(
								FString::Printf(TEXT("%s.%s"), *L->GetOwningNode()->GetName(), *L->PinName.ToString())));
						}
					}
					PinObj->SetArrayField(TEXT("linkedTo"), Links);
				}
				Pins.Add(MakeShared<FJsonValueObject>(PinObj));
			}
			ModObj->SetArrayField(TEXT("pins"), Pins);
			ModulesArr.Add(MakeShared<FJsonValueObject>(ModObj));
		}
	}

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	Res->SetStringField(TEXT("systemPath"), SystemPath);
	Res->SetStringField(TEXT("emitter"), Emitter ? Emitter->GetName() : TEXT(""));
	Res->SetNumberField(TEXT("moduleCount"), ModulesArr.Num());
	Res->SetArrayField(TEXT("modules"), ModulesArr);
	Res->SetNumberField(TEXT("dataInterfaceNodeCount"), DataInterfaceNodes.Num());
	Res->SetArrayField(TEXT("dataInterfaceNodes"), DataInterfaceNodes);
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::SetModuleEnabled(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (auto Err = RequireString(Params, TEXT("systemPath"), SystemPath)) return Err;
	FString ModuleName;
	if (auto Err = RequireString(Params, TEXT("moduleName"), ModuleName)) return Err;
	if (!Params->HasField(TEXT("enabled")))
	{
		return MCPError(TEXT("Missing required parameter 'enabled' (true|false)"));
	}
	const bool bEnabled = OptionalBool(Params, TEXT("enabled"), true);
	FString EmitterName = OptionalString(Params, TEXT("emitterName"), TEXT(""));
	int32 EmitterIndex = OptionalInt(Params, TEXT("emitterIndex"), 0);
	FString StackContext = OptionalString(Params, TEXT("stackContext"), TEXT("all"));

	UNiagaraSystem* System = Cast<UNiagaraSystem>(UEditorAssetLibrary::LoadAsset(SystemPath));
	if (!System) return MCPError(FString::Printf(TEXT("System not found: %s"), *SystemPath));

	UNiagaraEmitter* Emitter = nullptr;
	FGuid Version;
	FVersionedNiagaraEmitterData* Data = ResolveEmitter(System, EmitterName, EmitterIndex, Emitter, Version);
	if (!Data) return MCPError(TEXT("Emitter not resolved"));

	TArray<FScriptSlot> Scripts;
	CollectEmitterScripts(Data, StackContext, Scripts);

	int32 UpdatedCount = 0;
	TArray<FString> UpdatedContexts;
	for (const FScriptSlot& Slot : Scripts)
	{
		UNiagaraGraph* Graph = GraphOfScript(Slot.Script);
		if (!Graph) continue;
		for (UEdGraphNode* N : Graph->Nodes)
		{
			UNiagaraNodeFunctionCall* FC = Cast<UNiagaraNodeFunctionCall>(N);
			if (!FC) continue;
			const FString ModName = FC->GetFunctionName();
			if (!ModName.Equals(ModuleName, ESearchCase::IgnoreCase)) continue;
			FNiagaraStackGraphUtilities::SetModuleIsEnabled(*FC, bEnabled);
			++UpdatedCount;
			UpdatedContexts.Add(Slot.Context);
		}
	}

	if (UpdatedCount == 0)
	{
		return MCPError(FString::Printf(TEXT("Module '%s' not found in emitter '%s'"), *ModuleName, *EmitterName));
	}

	Emitter->PostEditChange();
	System->PostEditChange();
	System->RequestCompile(false);
	UEditorAssetLibrary::SaveLoadedAsset(System);

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	MCPSetUpdated(Res);
	Res->SetStringField(TEXT("systemPath"), SystemPath);
	Res->SetStringField(TEXT("emitter"), Emitter->GetName());
	Res->SetStringField(TEXT("moduleName"), ModuleName);
	Res->SetBoolField(TEXT("enabled"), bEnabled);
	Res->SetNumberField(TEXT("modulesUpdated"), UpdatedCount);
	Res->SetStringField(TEXT("stackContexts"), FString::Join(UpdatedContexts, TEXT(", ")));
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::DescribeScriptGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString ObjectPath;
	if (auto Err = RequireString(Params, TEXT("objectPath"), ObjectPath)) return Err;

	UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *ObjectPath);
	if (!Script)
	{
		return MCPError(FString::Printf(TEXT("NiagaraScript not found: %s"), *ObjectPath));
	}

	UNiagaraGraph* Graph = GraphOfScript(Script);
	if (!Graph) return MCPError(TEXT("Script has no source graph"));

	TArray<TSharedPtr<FJsonValue>> ModulesArr;
	TArray<TSharedPtr<FJsonValue>> DataInterfaceNodes;
	TArray<TSharedPtr<FJsonValue>> OutputNodes;
	TArray<TSharedPtr<FJsonValue>> AllNodes;
	for (UEdGraphNode* N : Graph->Nodes)
	{
		{
			TSharedPtr<FJsonObject> AllObj = MakeShared<FJsonObject>();
			AllObj->SetStringField(TEXT("class"), N->GetClass()->GetName());
			AllObj->SetStringField(TEXT("name"), N->GetName());
			if (UNiagaraNodeFunctionCall* AllFC = Cast<UNiagaraNodeFunctionCall>(N))
			{
				AllObj->SetStringField(TEXT("function"), AllFC->GetFunctionName());
				AllObj->SetBoolField(TEXT("isScriptCall"), AllFC->FunctionScript != nullptr);
			}
			if (FNameProperty* OpProp = CastField<FNameProperty>(
				N->GetClass()->FindPropertyByName(TEXT("OpName"))))
			{
				AllObj->SetStringField(TEXT("op"), OpProp->GetPropertyValue_InContainer(N).ToString());
			}
			TArray<TSharedPtr<FJsonValue>> AllPins;
			for (UEdGraphPin* Pin : N->Pins)
			{
				if (!Pin) continue;
				TSharedPtr<FJsonObject> PinObj = PinToJson(Pin);
				PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Output ? TEXT("output") : TEXT("input"));
				AllPins.Add(MakeShared<FJsonValueObject>(PinObj));
			}
			AllObj->SetArrayField(TEXT("pins"), AllPins);
			AllNodes.Add(MakeShared<FJsonValueObject>(AllObj));
		}
		if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(N))
		{
			DataInterfaceNodes.Add(DIInputNodeToJson(InputNode, TEXT("script")));
			continue;
		}
		if (UNiagaraNodeFunctionCall* FC = Cast<UNiagaraNodeFunctionCall>(N))
		{
			ModulesArr.Add(ModuleNodeToJson(FC, TEXT("script")));
			continue;
		}
		if (UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(N))
		{
			TSharedPtr<FJsonObject> OutObj = MakeShared<FJsonObject>();
			OutObj->SetStringField(TEXT("nodeName"), OutputNode->GetName());
			OutObj->SetNumberField(TEXT("usage"),
				static_cast<int32>(FNiagaraStackGraphUtilities::GetOutputNodeUsage(*OutputNode)));
			TArray<TSharedPtr<FJsonValue>> Pins;
			for (UEdGraphPin* Pin : OutputNode->Pins)
			{
				if (!Pin) continue;
				TSharedPtr<FJsonObject> PinObj = PinToJson(Pin);
				PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Output ? TEXT("output") : TEXT("input"));
				Pins.Add(MakeShared<FJsonValueObject>(PinObj));
			}
			OutObj->SetArrayField(TEXT("pins"), Pins);
			OutputNodes.Add(MakeShared<FJsonValueObject>(OutObj));
		}
	}

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	Res->SetStringField(TEXT("objectPath"), ObjectPath);
	Res->SetNumberField(TEXT("nodeCount"), Graph->Nodes.Num());
	Res->SetArrayField(TEXT("allNodes"), AllNodes);
	Res->SetArrayField(TEXT("modules"), ModulesArr);
	Res->SetArrayField(TEXT("dataInterfaceNodes"), DataInterfaceNodes);
	Res->SetArrayField(TEXT("outputNodes"), OutputNodes);
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::SetDiFunction(const TSharedPtr<FJsonObject>& Params)
{
	FString ObjectPath;
	if (auto Err = RequireString(Params, TEXT("objectPath"), ObjectPath)) return Err;
	FString NewFunction;
	if (auto Err = RequireString(Params, TEXT("newFunction"), NewFunction)) return Err;
	FString NodeName = OptionalString(Params, TEXT("nodeName"), TEXT(""));
	FString CurrentFunction = OptionalString(Params, TEXT("currentFunction"), TEXT(""));
	FString OutputTarget = OptionalString(Params, TEXT("outputTarget"), TEXT(""));
	FString DiClass = OptionalString(Params, TEXT("diClass"), TEXT("NiagaraDataInterfaceParticleRead"));
	const bool bDryRun = OptionalBool(Params, TEXT("dryRun"), false);

	UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *ObjectPath);
	if (!Script)
	{
		return MCPError(FString::Printf(TEXT("NiagaraScript not found: %s"), *ObjectPath));
	}
	UNiagaraGraph* Graph = GraphOfScript(Script);
	if (!Graph) return MCPError(TEXT("Script has no source graph"));

	UClass* DiUClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Niagara.%s"), *DiClass));
	if (!DiUClass)
	{
		return MCPError(FString::Printf(TEXT("Data interface class not found: %s"), *DiClass));
	}
	UNiagaraDataInterface* DI = NewObject<UNiagaraDataInterface>(GetTransientPackage(), DiUClass);
	if (!DI)
	{
		return MCPError(FString::Printf(TEXT("Failed to instantiate %s"), *DiClass));
	}

	TArray<FNiagaraFunctionSignature> Signatures;
	DI->GetFunctionSignatures(Signatures);
	const FNiagaraFunctionSignature* NewSig = nullptr;
	for (const FNiagaraFunctionSignature& S : Signatures)
	{
		if (S.Name.ToString().Equals(NewFunction, ESearchCase::IgnoreCase))
		{
			NewSig = &S;
			break;
		}
	}
	if (!NewSig)
	{
		return MCPError(FString::Printf(TEXT("Function '%s' not found on %s"), *NewFunction, *DiClass));
	}

	int32 Updated = 0;
	TArray<FString> UpdatedNodes;
	TArray<UNiagaraNodeFunctionCall*> Matched;
	TArray<TSharedPtr<FJsonValue>> Candidates;
	for (UEdGraphNode* N : Graph->Nodes)
	{
		UNiagaraNodeFunctionCall* FC = Cast<UNiagaraNodeFunctionCall>(N);
		// Only DI function calls (no backing script) are retargetable this way.
		if (!FC || FC->FunctionScript) continue;
		{
			TSharedPtr<FJsonObject> Cand = MakeShared<FJsonObject>();
			Cand->SetStringField(TEXT("nodeName"), N->GetName());
			Cand->SetStringField(TEXT("displayFunction"), FC->GetFunctionName());
			Cand->SetStringField(TEXT("signatureFunction"), FC->Signature.Name.ToString());
			TArray<TSharedPtr<FJsonValue>> OutLinks;
			for (UEdGraphPin* Pin : FC->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output) continue;
				for (UEdGraphPin* L : Pin->LinkedTo)
				{
					if (L && L->GetOwningNode())
					{
						OutLinks.Add(MakeShared<FJsonValueString>(
							FString::Printf(TEXT("%s.%s"), *L->GetOwningNode()->GetName(), *L->PinName.ToString())));
					}
				}
			}
			Cand->SetArrayField(TEXT("outputLinks"), OutLinks);
			Candidates.Add(MakeShared<FJsonValueObject>(Cand));
		}
		if (!NodeName.IsEmpty() && !N->GetName().Equals(NodeName, ESearchCase::IgnoreCase)) continue;
		const FString Cur = FC->GetFunctionName();
		if (!CurrentFunction.IsEmpty() && !Cur.Contains(CurrentFunction, ESearchCase::IgnoreCase)) continue;
		if (!OutputTarget.IsEmpty())
		{
			bool bFoundTarget = false;
			for (UEdGraphPin* Pin : FC->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output) continue;
				for (UEdGraphPin* L : Pin->LinkedTo)
				{
					if (L && L->GetOwningNode() &&
						FString::Printf(TEXT("%s.%s"), *L->GetOwningNode()->GetName(), *L->PinName.ToString())
							.Contains(OutputTarget, ESearchCase::IgnoreCase))
					{
						bFoundTarget = true;
						break;
					}
				}
				if (bFoundTarget) break;
			}
			if (!bFoundTarget) continue;
		}

		Matched.Add(FC);
	}

	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Res = MCPSuccess();
		Res->SetStringField(TEXT("objectPath"), ObjectPath);
		Res->SetNumberField(TEXT("candidateCount"), Candidates.Num());
		Res->SetArrayField(TEXT("candidates"), Candidates);
		Res->SetNumberField(TEXT("wouldUpdate"), Matched.Num());
		return MCPResult(Res);
	}

	for (UNiagaraNodeFunctionCall* FC : Matched)
	{
		const FString Cur = FC->GetFunctionName();
		FC->Modify();
		FC->Signature = *NewSig;
		FC->ReconstructNode();
		FC->PostEditChange();
		Graph->NotifyGraphChanged();
		++Updated;
		UpdatedNodes.Add(FString::Printf(TEXT("%s (%s -> %s)"), *FC->GetName(), *Cur, *NewFunction));
	}

	if (Updated == 0)
	{
		return MCPError(FString::Printf(TEXT("No matching DI function-call node in %s (nodeName='%s', currentFunction='%s')"),
			*ObjectPath, *NodeName, *CurrentFunction));
	}

	Script->MarkPackageDirty();
	UEditorAssetLibrary::SaveLoadedAsset(Script);

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	MCPSetUpdated(Res);
	Res->SetStringField(TEXT("objectPath"), ObjectPath);
	Res->SetStringField(TEXT("newFunction"), NewFunction);
	Res->SetNumberField(TEXT("nodesUpdated"), Updated);
	Res->SetStringField(TEXT("nodes"), FString::Join(UpdatedNodes, TEXT(", ")));
	return MCPResult(Res);
}

// ===========================================================================
// Generic Niagara graph authoring primitives
// ===========================================================================
namespace
{
	UNiagaraScript* LoadScriptAndGraph(const FString& ObjectPath, UNiagaraGraph*& OutGraph, FString& OutErr)
	{
		UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *ObjectPath);
		if (!Script)
		{
			OutErr = FString::Printf(TEXT("NiagaraScript not found: %s"), *ObjectPath);
			return nullptr;
		}
		OutGraph = GraphOfScript(Script);
		if (!OutGraph)
		{
			OutErr = TEXT("Script has no source graph");
			return nullptr;
		}
		return Script;
	}

	void SaveScriptPackage(UNiagaraScript* Script)
	{
		if (!Script) return;
		const FString PkgName = Script->GetPackage() ? Script->GetPackage()->GetName() : FString();
		if (!PkgName.IsEmpty())
		{
			UEditorAssetLibrary::SaveAsset(PkgName);
		}
	}

	UNiagaraNode* FindNiagaraNode(UNiagaraGraph* Graph, const FString& Name)
	{
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (N && N->GetName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Cast<UNiagaraNode>(N);
			}
		}
		return nullptr;
	}

	UEdGraphPin* FindNodePin(UEdGraphNode* Node, const FString& PinName, int32 Direction)
	{
		if (!Node) return nullptr;
		for (UEdGraphPin* P : Node->Pins)
		{
			if (!P || !P->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase)) continue;
			if (Direction < 0 || P->Direction == (Direction == 0 ? EGPD_Input : EGPD_Output)) return P;
		}
		return nullptr;
	}
}

TSharedPtr<FJsonValue> FNiagaraHandlers::GraphAddNode(const TSharedPtr<FJsonObject>& Params)
{
	FString ObjectPath;
	if (auto Err = RequireString(Params, TEXT("objectPath"), ObjectPath)) return Err;
	FString NodeClass;
	if (auto Err = RequireString(Params, TEXT("nodeClass"), NodeClass)) return Err;
	FString OpName = OptionalString(Params, TEXT("opName"), TEXT(""));
	FString Hlsl = OptionalString(Params, TEXT("hlsl"), TEXT(""));
	FString DiClass = OptionalString(Params, TEXT("diClass"), TEXT(""));
	FString DiFunction = OptionalString(Params, TEXT("diFunction"), TEXT(""));

	UNiagaraGraph* Graph = nullptr;
	FString Err;
	UNiagaraScript* Script = LoadScriptAndGraph(ObjectPath, Graph, Err);
	if (!Script) return MCPError(Err);

	UClass* Cls = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/NiagaraEditor.%s"), *NodeClass));
	if (!Cls || !Cls->IsChildOf(UNiagaraNode::StaticClass()))
	{
		return MCPError(FString::Printf(TEXT("Niagara node class not found: %s"), *NodeClass));
	}

	Graph->Modify();
	UEdGraphNode* Node = nullptr;
	if (Cls == UNiagaraNodeOp::StaticClass())
	{
		FGraphNodeCreator<UNiagaraNodeOp> Creator(*Graph);
		UNiagaraNodeOp* Op = Creator.CreateNode();
		if (!OpName.IsEmpty()) Op->OpName = FName(*OpName);
		Creator.Finalize();
		Op->ReconstructNode();
		Node = Op;
	}
	else if (Cls == UNiagaraNodeCustomHlsl::StaticClass())
	{
		FGraphNodeCreator<UNiagaraNodeCustomHlsl> Creator(*Graph);
		UNiagaraNodeCustomHlsl* Custom = Creator.CreateNode();
		if (!Hlsl.IsEmpty())
		{
			if (FStrProperty* SP = CastField<FStrProperty>(
				Custom->GetClass()->FindPropertyByName(TEXT("CustomHlsl"))))
			{
				Custom->Modify();
				SP->SetPropertyValue(SP->ContainerPtrToValuePtr<void>(Custom), Hlsl);
			}
		}
		Creator.Finalize();
		Custom->ReconstructNode();
		Node = Custom;
	}
	else if (Cls == UNiagaraNodeFunctionCall::StaticClass())
	{
		FGraphNodeCreator<UNiagaraNodeFunctionCall> Creator(*Graph);
		UNiagaraNodeFunctionCall* FC = Creator.CreateNode();
		if (!DiClass.IsEmpty() && !DiFunction.IsEmpty())
		{
			UClass* DiUClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Niagara.%s"), *DiClass));
			if (DiUClass)
			{
				UNiagaraDataInterface* DI = NewObject<UNiagaraDataInterface>(GetTransientPackage(), DiUClass);
				if (DI)
				{
					TArray<FNiagaraFunctionSignature> Sigs;
					DI->GetFunctionSignatures(Sigs);
					for (const FNiagaraFunctionSignature& S : Sigs)
					{
						if (S.Name.ToString().Equals(DiFunction, ESearchCase::IgnoreCase))
						{
							FC->Signature = S;
							break;
						}
					}
				}
			}
		}
		Creator.Finalize();
		FC->ReconstructNode();
		Node = FC;
	}
	else
	{
		// Generic path for node classes whose headers are private
		// (MapGet/MapSet/Convert/Reroute): create raw and allocate pins.
		Node = NewObject<UEdGraphNode>(Graph, Cls);
		if (Node)
		{
			Node->SetFlags(RF_Transactional);
			Graph->AddNode(Node, false, false);
			Node->AllocateDefaultPins();
			Node->ReconstructNode();
		}
	}
	if (!Node) return MCPError(TEXT("Failed to create node"));
	Node->PostEditChange();
	Graph->NotifyGraphChanged();
	SaveScriptPackage(Script);

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	MCPSetCreated(Res);
	Res->SetStringField(TEXT("objectPath"), ObjectPath);
	Res->SetStringField(TEXT("nodeClass"), Cls->GetName());
	Res->SetStringField(TEXT("nodeName"), Node->GetName());
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::GraphConnectPins(const TSharedPtr<FJsonObject>& Params)
{
	FString ObjectPath;
	if (auto Err = RequireString(Params, TEXT("objectPath"), ObjectPath)) return Err;
	FString FromNode;
	if (auto Err = RequireString(Params, TEXT("fromNode"), FromNode)) return Err;
	FString FromPin;
	if (auto Err = RequireString(Params, TEXT("fromPin"), FromPin)) return Err;
	FString ToNode;
	if (auto Err = RequireString(Params, TEXT("toNode"), ToNode)) return Err;
	FString ToPin;
	if (auto Err = RequireString(Params, TEXT("toPin"), ToPin)) return Err;

	UNiagaraGraph* Graph = nullptr;
	FString Err;
	UNiagaraScript* Script = LoadScriptAndGraph(ObjectPath, Graph, Err);
	if (!Script) return MCPError(Err);

	UEdGraphPin* From = FindNodePin(FindNiagaraNode(Graph, FromNode), FromPin, 1);
	UEdGraphPin* To = FindNodePin(FindNiagaraNode(Graph, ToNode), ToPin, 0);
	if (!From) return MCPError(FString::Printf(TEXT("Output pin '%s' not found on node '%s'"), *FromPin, *FromNode));
	if (!To) return MCPError(FString::Printf(TEXT("Input pin '%s' not found on node '%s'"), *ToPin, *ToNode));
	if (From->LinkedTo.Contains(To))
	{
		return MCPError(TEXT("Pins are already connected"));
	}

	Graph->Modify();
	From->Modify();
	To->Modify();
	From->MakeLinkTo(To);
	Graph->NotifyGraphChanged();
	SaveScriptPackage(Script);

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	MCPSetUpdated(Res);
	Res->SetStringField(TEXT("connection"), FString::Printf(TEXT("%s.%s -> %s.%s"), *FromNode, *FromPin, *ToNode, *ToPin));
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::GraphDisconnectPins(const TSharedPtr<FJsonObject>& Params)
{
	FString ObjectPath;
	if (auto Err = RequireString(Params, TEXT("objectPath"), ObjectPath)) return Err;
	FString NodeName;
	if (auto Err = RequireString(Params, TEXT("node"), NodeName)) return Err;
	FString PinName;
	if (auto Err = RequireString(Params, TEXT("pin"), PinName)) return Err;
	// -1 any, 0 input, 1 output
	const int32 Direction = OptionalInt(Params, TEXT("direction"), -1);

	UNiagaraGraph* Graph = nullptr;
	FString Err;
	UNiagaraScript* Script = LoadScriptAndGraph(ObjectPath, Graph, Err);
	if (!Script) return MCPError(Err);

	UEdGraphPin* Pin = FindNodePin(FindNiagaraNode(Graph, NodeName), PinName, Direction);
	if (!Pin) return MCPError(FString::Printf(TEXT("Pin '%s' not found on node '%s'"), *PinName, *NodeName));
	const int32 BrokenCount = Pin->LinkedTo.Num();

	Graph->Modify();
	Pin->Modify();
	Pin->BreakAllPinLinks(true);
	Graph->NotifyGraphChanged();
	SaveScriptPackage(Script);

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	MCPSetUpdated(Res);
	Res->SetStringField(TEXT("node"), NodeName);
	Res->SetStringField(TEXT("pin"), PinName);
	Res->SetNumberField(TEXT("linksBroken"), BrokenCount);
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::GraphSetPin(const TSharedPtr<FJsonObject>& Params)
{
	FString ObjectPath;
	if (auto Err = RequireString(Params, TEXT("objectPath"), ObjectPath)) return Err;
	FString NodeName;
	if (auto Err = RequireString(Params, TEXT("node"), NodeName)) return Err;
	FString PinName;
	if (auto Err = RequireString(Params, TEXT("pin"), PinName)) return Err;
	FString Value;
	if (auto Err = RequireString(Params, TEXT("value"), Value)) return Err;
	const int32 Direction = OptionalInt(Params, TEXT("direction"), 0);

	UNiagaraGraph* Graph = nullptr;
	FString Err;
	UNiagaraScript* Script = LoadScriptAndGraph(ObjectPath, Graph, Err);
	if (!Script) return MCPError(Err);

	UEdGraphPin* Pin = FindNodePin(FindNiagaraNode(Graph, NodeName), PinName, Direction);
	if (!Pin) return MCPError(FString::Printf(TEXT("Pin '%s' not found on node '%s'"), *PinName, *NodeName));

	const bool bWasLinked = Pin->LinkedTo.Num() > 0;
	Graph->Modify();
	Pin->Modify();
	if (bWasLinked)
	{
		Pin->BreakAllPinLinks(true);
	}
	Pin->DefaultValue = Value;
	Graph->NotifyGraphChanged();
	SaveScriptPackage(Script);

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	MCPSetUpdated(Res);
	Res->SetStringField(TEXT("node"), NodeName);
	Res->SetStringField(TEXT("pin"), PinName);
	Res->SetStringField(TEXT("value"), Value);
	Res->SetBoolField(TEXT("brokeLinks"), bWasLinked);
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::GraphSetNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString ObjectPath;
	if (auto Err = RequireString(Params, TEXT("objectPath"), ObjectPath)) return Err;
	FString NodeName;
	if (auto Err = RequireString(Params, TEXT("node"), NodeName)) return Err;
	FString PropertyName;
	if (auto Err = RequireString(Params, TEXT("propertyName"), PropertyName)) return Err;
	FString Value;
	if (auto Err = RequireString(Params, TEXT("value"), Value)) return Err;

	UNiagaraGraph* Graph = nullptr;
	FString Err;
	UNiagaraScript* Script = LoadScriptAndGraph(ObjectPath, Graph, Err);
	if (!Script) return MCPError(Err);

	UNiagaraNode* Node = FindNiagaraNode(Graph, NodeName);
	if (!Node) return MCPError(FString::Printf(TEXT("Node '%s' not found"), *NodeName));

	FProperty* Prop = Node->GetClass()->FindPropertyByName(*PropertyName);
	if (!Prop)
	{
		return MCPError(FString::Printf(TEXT("Property '%s' not found on node '%s'"), *PropertyName, *NodeName));
	}

	Node->Modify();
	bool bSet = false;
	if (FBoolProperty* BP = CastField<FBoolProperty>(Prop)) { BP->SetPropertyValue_InContainer(Node, Value.ToBool()); bSet = true; }
	else if (FIntProperty* IP = CastField<FIntProperty>(Prop)) { IP->SetPropertyValue_InContainer(Node, FCString::Atoi(*Value)); bSet = true; }
	else if (FFloatProperty* FP = CastField<FFloatProperty>(Prop)) { FP->SetPropertyValue_InContainer(Node, FCString::Atof(*Value)); bSet = true; }
	else if (FDoubleProperty* DP = CastField<FDoubleProperty>(Prop)) { DP->SetPropertyValue_InContainer(Node, FCString::Atod(*Value)); bSet = true; }
	else if (FNameProperty* NP = CastField<FNameProperty>(Prop)) { NP->SetPropertyValue_InContainer(Node, FName(*Value)); bSet = true; }
	else if (FStrProperty* SP = CastField<FStrProperty>(Prop)) { SP->SetPropertyValue_InContainer(Node, Value); bSet = true; }
	else if (FTextProperty* TP = CastField<FTextProperty>(Prop)) { TP->SetPropertyValue_InContainer(Node, FText::FromString(Value)); bSet = true; }

	if (!bSet)
	{
		return MCPError(FString::Printf(TEXT("Unsupported property type for '%s'"), *PropertyName));
	}

	Node->PostEditChange();
	Graph->NotifyGraphChanged();
	SaveScriptPackage(Script);

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	MCPSetUpdated(Res);
	Res->SetStringField(TEXT("node"), NodeName);
	Res->SetStringField(TEXT("propertyName"), PropertyName);
	Res->SetStringField(TEXT("value"), Value);
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FNiagaraHandlers::GraphRemoveNode(const TSharedPtr<FJsonObject>& Params)
{
	FString ObjectPath;
	if (auto Err = RequireString(Params, TEXT("objectPath"), ObjectPath)) return Err;
	FString NodeName;
	if (auto Err = RequireString(Params, TEXT("node"), NodeName)) return Err;

	UNiagaraGraph* Graph = nullptr;
	FString Err;
	UNiagaraScript* Script = LoadScriptAndGraph(ObjectPath, Graph, Err);
	if (!Script) return MCPError(Err);

	UNiagaraNode* Node = FindNiagaraNode(Graph, NodeName);
	if (!Node) return MCPError(FString::Printf(TEXT("Node '%s' not found"), *NodeName));

	Graph->Modify();
	Node->Modify();
	Graph->RemoveNode(Node);
	Graph->NotifyGraphChanged();
	SaveScriptPackage(Script);

	TSharedPtr<FJsonObject> Res = MCPSuccess();
	MCPSetUpdated(Res);
	Res->SetStringField(TEXT("removedNode"), NodeName);
	return MCPResult(Res);
}
