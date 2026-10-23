// Graph user parameters, PCG Graph Instance assets and their overrides, and Get Graph Parameter nodes by name (#1264).
#include "PCGHandlers.h"
#include "PCGHandlers_Internal.h"
#include "HandlerUtils.h"
#include "HandlerAssetCreate.h"
#include "HandlerPackageSave.h"
#include "HandlerJsonProperty.h"
#include "Elements/PCGUserParameterGet.h"
#include "ScopedTransaction.h"
#include "StructUtils/PropertyBag.h"

namespace
{
	struct FMCPParamType
	{
		EPropertyBagPropertyType Type = EPropertyBagPropertyType::None;
		const UObject* TypeObject = nullptr;
	};

	/** A parameter type by name: bool, int32, int64, float, double, name, string, text, vector, vector2d, vector4, rotator,
	 *  transform, quat, color, object, class, enum (with typeObject), struct (with typeObject). */
	bool MCPParseParamType(const FString& InType, const FString& TypeObjectPath, FMCPParamType& Out, FString& Error)
	{
		const FString T = InType.ToLower();
		auto Struct = [&Out](UScriptStruct* S) { Out.Type = EPropertyBagPropertyType::Struct; Out.TypeObject = S; return true; };
		if (T == TEXT("bool") || T == TEXT("boolean")) Out.Type = EPropertyBagPropertyType::Bool;
		else if (T == TEXT("int") || T == TEXT("int32") || T == TEXT("integer")) Out.Type = EPropertyBagPropertyType::Int32;
		else if (T == TEXT("int64")) Out.Type = EPropertyBagPropertyType::Int64;
		else if (T == TEXT("float")) Out.Type = EPropertyBagPropertyType::Float;
		else if (T == TEXT("double") || T == TEXT("number")) Out.Type = EPropertyBagPropertyType::Double;
		else if (T == TEXT("name")) Out.Type = EPropertyBagPropertyType::Name;
		else if (T == TEXT("string")) Out.Type = EPropertyBagPropertyType::String;
		else if (T == TEXT("text")) Out.Type = EPropertyBagPropertyType::Text;
		else if (T == TEXT("vector")) return Struct(TBaseStructure<FVector>::Get());
		else if (T == TEXT("vector2d") || T == TEXT("vector2")) return Struct(TBaseStructure<FVector2D>::Get());
		else if (T == TEXT("vector4")) return Struct(TBaseStructure<FVector4>::Get());
		else if (T == TEXT("rotator")) return Struct(TBaseStructure<FRotator>::Get());
		else if (T == TEXT("transform")) return Struct(TBaseStructure<FTransform>::Get());
		else if (T == TEXT("quat")) return Struct(TBaseStructure<FQuat>::Get());
		else if (T == TEXT("color") || T == TEXT("linearcolor")) return Struct(TBaseStructure<FLinearColor>::Get());
		else if (T == TEXT("object") || T == TEXT("softobject") || T == TEXT("class") || T == TEXT("softclass"))
		{
			const bool bClass = T.EndsWith(TEXT("class"));
			Out.Type = bClass ? EPropertyBagPropertyType::SoftClass : EPropertyBagPropertyType::SoftObject;
			Out.TypeObject = TypeObjectPath.IsEmpty() ? UObject::StaticClass() : MCPResolveClass(TypeObjectPath);
			if (!Out.TypeObject) { Error = FString::Printf(TEXT("class '%s' not found"), *TypeObjectPath); return false; }
		}
		else if (T == TEXT("enum"))
		{
			Out.Type = EPropertyBagPropertyType::Enum;
			Out.TypeObject = TypeObjectPath.IsEmpty() ? nullptr : FindFirstObject<UEnum>(*TypeObjectPath, EFindFirstObjectOptions::NativeFirst);
			if (!Out.TypeObject && !TypeObjectPath.IsEmpty()) Out.TypeObject = LoadObject<UEnum>(nullptr, *TypeObjectPath);
			if (!Out.TypeObject) { Error = TEXT("an enum parameter needs typeObject naming the enum"); return false; }
		}
		else if (T == TEXT("struct"))
		{
			UScriptStruct* S = TypeObjectPath.IsEmpty() ? nullptr : FindFirstObject<UScriptStruct>(*TypeObjectPath, EFindFirstObjectOptions::NativeFirst);
			if (!S && !TypeObjectPath.IsEmpty()) S = LoadObject<UScriptStruct>(nullptr, *TypeObjectPath);
			if (!S) { Error = TEXT("a struct parameter needs typeObject naming the struct"); return false; }
			return Struct(S);
		}
		else
		{
			Error = FString::Printf(TEXT("unknown type '%s'"), *InType);
			return false;
		}
		return true;
	}

	/** One {name, type, typeObject?, default?} entry, parsed; the default is checked by the caller on a scratch bag. */
	struct FMCPParamEntry
	{
		FName Name;
		FMCPParamType Type;
		bool bHasType = false;
		TSharedPtr<FJsonValue> Default;
	};

	TSharedPtr<FJsonObject> MCPGraphParamsResult(UPCGGraph* Graph)
	{
		auto Result = MCPSuccess();
		Result->SetStringField(TEXT("assetPath"), Graph->GetPathName());
		Result->SetArrayField(TEXT("parameters"), MCPPCG::DescribeParameterBag(Graph->GetUserParametersStruct()));
		return Result;
	}

	/** The parameters of Graph as set_pcg_graph_parameters entries, for a rollback. */
	TArray<TSharedPtr<FJsonValue>> MCPGraphParamsAsEntries(const FInstancedPropertyBag* Bag)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		const UPropertyBag* BagStruct = Bag ? Bag->GetPropertyBagStruct() : nullptr;
		if (!BagStruct) return Out;
		for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
		{
			if (!Desc.CachedProperty) continue;
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Desc.Name.ToString());
			FString Text;
			Desc.CachedProperty->ExportTextItem_Direct(Text, Desc.CachedProperty->ContainerPtrToValuePtr<void>(Bag->GetValue().GetMemory()), nullptr, nullptr, PPF_None);
			Entry->SetStringField(TEXT("default"), Text);
			Out.Add(MakeShared<FJsonValueObject>(Entry));
		}
		return Out;
	}
}

TSharedPtr<FJsonValue> FPCGHandlers::SetGraphParameters(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Err = RequireString(Params, TEXT("assetPath"), AssetPath)) return Err;
	const TArray<TSharedPtr<FJsonValue>>* ParamsArr = nullptr;
	const bool bHasParams = TryGetArrayParam(Params, TEXT("graphParameters"), ParamsArr) && ParamsArr;
	const TArray<TSharedPtr<FJsonValue>>* RemoveArr = nullptr;
	const bool bHasRemove = TryGetArrayParam(Params, TEXT("removeParameters"), RemoveArr) && RemoveArr;
	if ((!bHasParams || ParamsArr->Num() == 0) && (!bHasRemove || RemoveArr->Num() == 0))
	{
		return MCPError(TEXT("Pass graphParameters [{name, type?, typeObject?, default?}] and/or removeParameters [names]."));
	}

	UPCGGraph* Graph = LoadAssetByPath<UPCGGraph>(AssetPath);
	if (!Graph) return MCPAssetLoadError(AssetPath, TEXT("PCGGraph"));
	const FInstancedPropertyBag* Current = Graph->GetUserParametersStruct();
	const UPropertyBag* CurrentStruct = Current ? Current->GetPropertyBagStruct() : nullptr;

	TArray<FString> Problems;
	TArray<FName> Removes;
	if (bHasRemove)
	{
		for (const TSharedPtr<FJsonValue>& V : *RemoveArr)
		{
			FString Name;
			if (!V.IsValid() || !V->TryGetString(Name) || Name.IsEmpty()) continue;
			if (!CurrentStruct || !CurrentStruct->FindPropertyDescByName(FName(*Name))) Problems.Add(FString::Printf(TEXT("removeParameters: '%s' is not a parameter"), *Name));
			else Removes.Add(FName(*Name));
		}
	}
	TArray<FMCPParamEntry> Entries;
	if (bHasParams)
	{
		for (const TSharedPtr<FJsonValue>& V : *ParamsArr)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			FString Name, Type, TypeObject;
			if (!V.IsValid() || !V->TryGetObject(Obj) || !Obj || !(*Obj)->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
			{
				Problems.Add(TEXT("every parameter needs a name"));
				continue;
			}
			FMCPParamEntry Entry;
			Entry.Name = FName(*Name);
			(*Obj)->TryGetStringField(TEXT("typeObject"), TypeObject);
			if ((*Obj)->TryGetStringField(TEXT("type"), Type) && !Type.IsEmpty())
			{
				FString TypeError;
				if (!MCPParseParamType(Type, TypeObject, Entry.Type, TypeError))
				{
					Problems.Add(FString::Printf(TEXT("'%s': %s"), *Name, *TypeError));
					continue;
				}
				Entry.bHasType = true;
			}
			else if (!CurrentStruct || !CurrentStruct->FindPropertyDescByName(Entry.Name))
			{
				Problems.Add(FString::Printf(TEXT("'%s' is new, so it needs a type"), *Name));
				continue;
			}
			Entry.Default = (*Obj)->TryGetField(TEXT("default"));
			Entries.Add(Entry);
		}
	}

	// The whole change runs on a scratch copy first, so a bad default refuses the call with nothing changed.
	auto Apply = [&Entries, &Removes](FInstancedPropertyBag& Bag, TArray<FString>& OutProblems)
	{
		for (const FName& Name : Removes) Bag.RemovePropertyByName(Name);
		for (const FMCPParamEntry& Entry : Entries)
		{
			const UPropertyBag* BagStruct = Bag.GetPropertyBagStruct();
			const FPropertyBagPropertyDesc* Desc = BagStruct ? BagStruct->FindPropertyDescByName(Entry.Name) : nullptr;
			const bool bRetype = Entry.bHasType && (!Desc || Desc->ValueType != Entry.Type.Type || Desc->ValueTypeObject != Entry.Type.TypeObject);
			if (bRetype) Bag.AddProperty(Entry.Name, Entry.Type.Type, Entry.Type.TypeObject, /*bOverwrite*/ true);
			if (!Entry.Default.IsValid()) continue;
			BagStruct = Bag.GetPropertyBagStruct();
			Desc = BagStruct ? BagStruct->FindPropertyDescByName(Entry.Name) : nullptr;
			FString ValueError;
			if (!Desc || !Desc->CachedProperty || !MCPJsonProperty::SetJsonOnProperty(const_cast<FProperty*>(Desc->CachedProperty),
				Desc->CachedProperty->ContainerPtrToValuePtr<void>(Bag.GetMutableValue().GetMemory()), Entry.Default, ValueError))
			{
				OutProblems.Add(FString::Printf(TEXT("'%s' default: %s"), *Entry.Name.ToString(), Desc ? *ValueError : TEXT("the parameter could not be added")));
			}
		}
	};
	if (Problems.Num() == 0)
	{
		FInstancedPropertyBag Scratch = Current ? *Current : FInstancedPropertyBag();
		Apply(Scratch, Problems);
	}
	if (Problems.Num() > 0)
	{
		return MCPError(FString::Printf(TEXT("Nothing was changed: %s."), *FString::Join(Problems, TEXT("; "))));
	}
	if (auto Blocked = MCPAssetWriteBlockedError(Graph, AssetPath, TEXT("change this PCG graph's parameters"))) return Blocked;

	const TArray<TSharedPtr<FJsonValue>> PreviousEntries = MCPGraphParamsAsEntries(Current);
	TArray<TSharedPtr<FJsonValue>> PreviousNames;
	for (const TSharedPtr<FJsonValue>& E : PreviousEntries) PreviousNames.Add(MakeShared<FJsonValueString>(E->AsObject()->GetStringField(TEXT("name"))));

	FScopedTransaction Transaction(NSLOCTEXT("UEMCPBridge", "SetPCGGraphParameters", "Set PCG Graph Parameters"));
	Graph->Modify();
	TArray<FString> Unused;
	Graph->UpdateUserParametersStruct([&Apply, &Unused](FInstancedPropertyBag& Bag) { Apply(Bag, Unused); });
	Graph->PostEditChange();
	Graph->MarkPackageDirty();
	FString SaveError;
	const bool bSaved = SaveAssetPackageChecked(Graph, SaveError);

	auto Result = MCPGraphParamsResult(Graph);
	MCPSetUpdated(Result);
	Result->SetArrayField(TEXT("previousParameters"), PreviousEntries);
	// Parameters this call added are removed; the ones it changed get their old defaults back, not their old types.
	TSet<FString> Had;
	for (const TSharedPtr<FJsonValue>& N : PreviousNames) Had.Add(N->AsString());
	TArray<TSharedPtr<FJsonValue>> Added;
	for (const FMCPParamEntry& Entry : Entries) if (!Had.Contains(Entry.Name.ToString())) Added.Add(MakeShared<FJsonValueString>(Entry.Name.ToString()));
	TArray<TSharedPtr<FJsonValue>> Restore;
	for (const TSharedPtr<FJsonValue>& E : PreviousEntries)
	{
		const FString Name = E->AsObject()->GetStringField(TEXT("name"));
		if (Entries.ContainsByPredicate([&Name](const FMCPParamEntry& X) { return X.Name.ToString() == Name; })) Restore.Add(E);
	}
	if (Restore.Num() == 0 && Added.Num() == 0)
	{
		MCPSetNoRollback(Result, TEXT("This call only removed parameters, and removed parameters do not come back; previousParameters lists them."));
		MCPNoteSaveOutcome(Result, AssetPath, bSaved, SaveError);
		return MCPResult(Result);
	}
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("assetPath"), Graph->GetPathName());
	Payload->SetArrayField(TEXT("graphParameters"), Restore);
	Payload->SetArrayField(TEXT("removeParameters"), Added);
	MCPSetRollback(Result, TEXT("set_pcg_graph_parameters"), Payload);
	const bool bLossy = Removes.Num() > 0 || Entries.ContainsByPredicate([](const FMCPParamEntry& X) { return X.bHasType; });
	Result->SetBoolField(TEXT("rollbackLossy"), bLossy);
	if (bLossy)
	{
		Result->SetStringField(TEXT("rollbackNote"), TEXT("Removed parameters do not come back, and a parameter whose type changed keeps the new type; previousParameters lists what was there."));
	}
	MCPNoteSaveOutcome(Result, AssetPath, bSaved, SaveError);
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FPCGHandlers::CreateGraphInstance(const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (auto Err = RequireString(Params, TEXT("name"), Name)) return Err;
	FString GraphPath;
	if (auto Err = RequireString(Params, TEXT("graphPath"), GraphPath)) return Err;
	const FString PackagePath = OptionalString(Params, TEXT("packagePath"), TEXT("/Game/PCG"));
	const FString OnConflict = OptionalString(Params, TEXT("onConflict"), TEXT("skip"));
	const TSharedPtr<FJsonObject>* Overrides = nullptr;
	const bool bHasOverrides = TryGetObjectParam(Params, TEXT("overrides"), Overrides) && Overrides && (*Overrides)->Values.Num() > 0;

	UPCGGraphInterface* Source = LoadAssetByPath<UPCGGraphInterface>(GraphPath);
	if (!Source) return MCPAssetLoadError(GraphPath, TEXT("PCGGraph or PCGGraphInstance"));

	auto Created = MCPCreateAssetIdempotentNewObject<UPCGGraphInstance>(Name, PackagePath, OnConflict, TEXT("PCGGraphInstance"));
	if (Created.EarlyReturn) return Created.EarlyReturn;
	UPCGGraphInstance* Instance = Created.Asset;
	Instance->SetGraph(Source);

	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	if (bHasOverrides)
	{
		TSharedPtr<FJsonObject> Previous;
		FString Error;
		if (!MCPPCG::ApplyInstanceOverrides(Instance, *Overrides, {}, Previous, Error))
		{
			// The asset exists but carries none of the overrides; say so rather than leave it unreported.
			Result = MCPErrorObject(Error);
			Result->SetStringField(TEXT("createdWithoutOverrides"), Instance->GetPathName());
		}
	}
	Instance->PostEditChange();
	Instance->MarkPackageDirty();
	FString SaveError;
	const bool bSaved = SaveAssetPackageChecked(Instance, SaveError);
	Result->SetStringField(TEXT("path"), Instance->GetPathName());
	Result->SetStringField(TEXT("graph"), Source->GetPathName());
	Result->SetArrayField(TEXT("parameters"), MCPPCG::DescribeParameterBag(Instance->GetUserParametersStruct(), Instance));
	MCPSetDeleteAssetRollback(Result, Instance->GetPathName());
	MCPNoteSaveOutcome(Result, Instance->GetPathName(), bSaved, SaveError);
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FPCGHandlers::SetGraphInstanceOverrides(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Err = RequireString(Params, TEXT("assetPath"), AssetPath)) return Err;
	const TSharedPtr<FJsonObject>* Overrides = nullptr;
	if (!TryGetObjectParam(Params, TEXT("overrides"), Overrides) || !Overrides || (*Overrides)->Values.Num() == 0)
	{
		return MCPError(TEXT("Missing 'overrides': {parameterName: value}; null clears that override."));
	}
	UPCGGraphInstance* Instance = LoadAssetByPath<UPCGGraphInstance>(AssetPath);
	if (!Instance) return MCPAssetLoadError(AssetPath, TEXT("PCGGraphInstance"));
	if (auto Blocked = MCPAssetWriteBlockedError(Instance, AssetPath, TEXT("change this graph instance's overrides"))) return Blocked;

	FScopedTransaction Transaction(NSLOCTEXT("UEMCPBridge", "SetPCGGraphInstanceOverrides", "Set PCG Graph Instance Overrides"));
	TSharedPtr<FJsonObject> Previous;
	FString Error;
	if (!MCPPCG::ApplyInstanceOverrides(Instance, *Overrides, {}, Previous, Error)) return MCPError(Error);
	Instance->PostEditChange();
	Instance->MarkPackageDirty();
	FString SaveError;
	const bool bSaved = SaveAssetPackageChecked(Instance, SaveError);

	auto Result = MCPSuccess();
	MCPSetUpdated(Result);
	Result->SetStringField(TEXT("assetPath"), Instance->GetPathName());
	Result->SetObjectField(TEXT("previous"), Previous);
	Result->SetArrayField(TEXT("parameters"), MCPPCG::DescribeParameterBag(Instance->GetUserParametersStruct(), Instance));
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("assetPath"), Instance->GetPathName());
	Payload->SetObjectField(TEXT("overrides"), Previous);
	MCPSetRollback(Result, TEXT("set_pcg_graph_instance_overrides"), Payload);
	MCPNoteSaveOutcome(Result, AssetPath, bSaved, SaveError);
	return MCPResult(Result);
}

// A Get Graph Parameter node is bound by a GUID no export carries, so it travels as the parameter's name.
void FPCGHandlers::ExportUserParameterNode(const UPCGSettings* Settings, const TSharedPtr<FJsonObject>& Node)
{
	if (const UPCGUserParameterGetSettings* Get = Cast<UPCGUserParameterGetSettings>(Settings))
	{
		const FNameProperty* NameProp = CastField<FNameProperty>(Get->GetClass()->FindPropertyByName(TEXT("PropertyName")));
		if (NameProp) Node->SetStringField(TEXT("parameter"), NameProp->GetPropertyValue_InContainer(Get).ToString());
	}
}

bool FPCGHandlers::ImportUserParameterNode(UPCGSettings* Settings, const TSharedPtr<FJsonObject>& Node, FString& Error)
{
	FString Parameter;
	if (!Node->TryGetStringField(TEXT("parameter"), Parameter)) return true;
	UPCGUserParameterGetSettings* Get = Cast<UPCGUserParameterGetSettings>(Settings);
	UPCGGraph* Graph = Settings->GetTypedOuter<UPCGGraph>();
	const FInstancedPropertyBag* Bag = Graph ? Graph->GetUserParametersStruct() : nullptr;
	const UPropertyBag* BagStruct = Bag ? Bag->GetPropertyBagStruct() : nullptr;
	const FPropertyBagPropertyDesc* Desc = BagStruct ? BagStruct->FindPropertyDescByName(FName(*Parameter)) : nullptr;
	FStructProperty* GuidProp = Get ? CastField<FStructProperty>(Get->GetClass()->FindPropertyByName(TEXT("PropertyGuid"))) : nullptr;
	FNameProperty* NameProp = Get ? CastField<FNameProperty>(Get->GetClass()->FindPropertyByName(TEXT("PropertyName"))) : nullptr;
	if (!Get || !GuidProp || !NameProp)
	{
		Error = TEXT("'parameter' is only for a PCGUserParameterGetSettings node");
		return false;
	}
	if (!Desc)
	{
		Error = FString::Printf(TEXT("the graph has no parameter '%s'; add it with set_graph_parameters first"), *Parameter);
		return false;
	}
	*GuidProp->ContainerPtrToValuePtr<FGuid>(Get) = Desc->ID;
	NameProp->SetPropertyValue_InContainer(Get, Desc->Name);
	if (UPCGNode* PCGNode = Settings->GetTypedOuter<UPCGNode>()) PCGNode->UpdateAfterSettingsChangeDuringCreation();
	return true;
}
