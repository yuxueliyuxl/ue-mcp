#include "PCGHandlers.h"
#include "PCGHandlers_Internal.h"
#include "HandlerUtils.h"
#include "HandlerPackageSave.h"
#include "HandlerJsonProperty.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSubgraph.h"

// #1253: assigning a subgraph by reflection skips SetSubgraph, so the instance never builds its parameter bag.

namespace
{
	struct FMCPSubgraphNode
	{
		UPCGGraph* Graph = nullptr;
		UPCGNode* Node = nullptr;
		UPCGSubgraphSettings* Settings = nullptr;
	};

	TSharedPtr<FJsonValue> MCPResolveSubgraphNode(const TSharedPtr<FJsonObject>& Params, FMCPSubgraphNode& Out)
	{
		FString AssetPath;
		if (auto Err = RequireString(Params, TEXT("assetPath"), AssetPath)) return Err;
		FString NodeName;
		if (auto Err = RequireString(Params, TEXT("nodeName"), NodeName)) return Err;

		Out.Graph = LoadAssetByPath<UPCGGraph>(AssetPath);
		if (!Out.Graph) return MCPAssetLoadError(AssetPath, TEXT("PCGGraph"));
		Out.Node = MCPPCG::FindNode(Out.Graph, NodeName);
		if (!Out.Node) return MCPPCG::NodeNotFoundError(Out.Graph, NodeName);
		Out.Settings = Cast<UPCGSubgraphSettings>(const_cast<UPCGSettings*>(Out.Node->GetSettings()));
		if (!Out.Settings)
		{
			return MCPError(FString::Printf(TEXT("Node %s is a %s, not a Subgraph node."), *NodeName,
				Out.Node->GetSettings() ? *Out.Node->GetSettings()->GetClass()->GetName() : TEXT("node without settings")));
		}
		return nullptr;
	}

	FString MCPSubgraphPathOf(const UPCGSubgraphSettings* Settings)
	{
		const UPCGGraphInterface* Sub = Settings && Settings->SubgraphInstance ? Settings->SubgraphInstance->Graph.Get() : nullptr;
		return Sub ? Sub->GetPathName() : FString();
	}

	/** Every user parameter of the node's graph instance: name, type, current value, and whether this node overrides it. */
	TArray<TSharedPtr<FJsonValue>> MCPDescribeSubgraphParameters(UPCGSubgraphSettings* Settings)
	{
		UPCGGraphInstance* Instance = Settings ? Settings->SubgraphInstance.Get() : nullptr;
		return Instance ? MCPPCG::DescribeParameterBag(Instance->GetUserParametersStruct(), Instance) : TArray<TSharedPtr<FJsonValue>>();
	}

	void MCPCommitSubgraphEdit(const FMCPSubgraphNode& Target, const TSharedPtr<FJsonObject>& Result)
	{
		Target.Settings->PostEditChange();
		Target.Graph->PostEditChange();
		Target.Graph->MarkPackageDirty();
		FString SaveError;
		const bool bSaved = SaveAssetPackageChecked(Target.Graph, SaveError);
		MCPNoteSaveOutcome(Result, Target.Graph->GetPathName(), bSaved, SaveError);
	}
	bool MCPApplySubgraphParameters(const FMCPSubgraphNode& Target, const TSharedPtr<FJsonObject>& Values,
		TSharedPtr<FJsonObject>& PreviousValues, FString& Error)
	{
		if (!MCPPCG::ApplyInstanceOverrides(Target.Settings->SubgraphInstance.Get(), Values, { Target.Graph, Target.Settings }, PreviousValues, Error))
		{
			Error += TEXT(" set_subgraph lists the parameters.");
			return false;
		}
		return true;
	}

}

TSharedPtr<FJsonValue> FPCGHandlers::SetSubgraph(const TSharedPtr<FJsonObject>& Params)
{
	// An empty path is a request to clear the subgraph, so only absence is an error.
	FString SubgraphPath;
	const bool bHasSubgraphPath = TryGetStringParam(Params, TEXT("subgraphPath"), SubgraphPath);
	FMCPSubgraphNode Target;
	if (auto Err = MCPResolveSubgraphNode(Params, Target)) return Err;
	if (!bHasSubgraphPath)
	{
		return MCPError(TEXT("Missing required parameter 'subgraphPath' (pass \"\" to clear the subgraph)"));
	}

	UPCGGraphInterface* Subgraph = nullptr;
	if (!SubgraphPath.IsEmpty())
	{
		Subgraph = LoadAssetByPath<UPCGGraphInterface>(SubgraphPath);
		if (!Subgraph) return MCPAssetLoadError(SubgraphPath, TEXT("PCGGraph or PCGGraphInstance"));
	}

	const FString Previous = MCPSubgraphPathOf(Target.Settings);
	const FString Next = Subgraph ? Subgraph->GetPathName() : FString();
	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("nodeName"), Target.Node->GetName());
	Result->SetStringField(TEXT("previousSubgraph"), Previous);
	if (Previous == Next)
	{
		Result->SetBoolField(TEXT("unchanged"), true);
		Result->SetBoolField(TEXT("updated"), false);
	}
	else
	{
		Target.Graph->Modify();
		Target.Settings->Modify();
		Target.Settings->SetSubgraph(Subgraph);
		Target.Node->UpdateAfterSettingsChangeDuringCreation();
		MCPSetUpdated(Result);
		MCPCommitSubgraphEdit(Target, Result);

		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("assetPath"), Target.Graph->GetPathName());
		Payload->SetStringField(TEXT("nodeName"), Target.Node->GetName());
		Payload->SetStringField(TEXT("subgraphPath"), Previous);
		MCPSetRollback(Result, TEXT("set_pcg_subgraph"), Payload);
		Result->SetBoolField(TEXT("rollbackLossy"), true);
		Result->SetStringField(TEXT("rollbackNote"), TEXT("Restores the previous subgraph; parameter overrides set on this node for the new subgraph are not carried back."));
	}
	Result->SetStringField(TEXT("subgraph"), Next);
	Result->SetStringField(TEXT("nodeTitle"), Target.Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());
	Result->SetArrayField(TEXT("inputPins"), MCPPCG::PinLabelsJson(Target.Node->GetInputPins()));
	Result->SetArrayField(TEXT("outputPins"), MCPPCG::PinLabelsJson(Target.Node->GetOutputPins()));
	Result->SetArrayField(TEXT("parameters"), MCPDescribeSubgraphParameters(Target.Settings));
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FPCGHandlers::SetSubgraphParameters(const TSharedPtr<FJsonObject>& Params)
{
	const TSharedPtr<FJsonObject>* Values = nullptr;
	const bool bHasValues = TryGetObjectParam(Params, TEXT("parameters"), Values);
	FMCPSubgraphNode Target;
	if (auto Err = MCPResolveSubgraphNode(Params, Target)) return Err;
	if (!bHasValues || !Values || !Values->IsValid() || (*Values)->Values.Num() == 0)
	{
		return MCPError(TEXT("Missing 'parameters': an object of {parameterName: value}; null clears that override."));
	}

	TSharedPtr<FJsonObject> PreviousValues;
	FString Error;
	if (!MCPApplySubgraphParameters(Target, *Values, PreviousValues, Error)) return MCPError(Error);

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("nodeName"), Target.Node->GetName());
	Result->SetStringField(TEXT("subgraph"), MCPSubgraphPathOf(Target.Settings));
	Result->SetObjectField(TEXT("previous"), PreviousValues);
	MCPSetUpdated(Result);
	MCPCommitSubgraphEdit(Target, Result);
	Result->SetArrayField(TEXT("parameters"), MCPDescribeSubgraphParameters(Target.Settings));

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("assetPath"), Target.Graph->GetPathName());
	Payload->SetStringField(TEXT("nodeName"), Target.Node->GetName());
	Payload->SetObjectField(TEXT("parameters"), PreviousValues);
	MCPSetRollback(Result, TEXT("set_pcg_subgraph_parameters"), Payload);
	return MCPResult(Result);
}

// Export the graph asset and overridden values, never a path to the node-owned instance.
void FPCGHandlers::ExportSubgraphSettings(const UPCGSettings* Settings, const TSharedPtr<FJsonObject>& Node)
{
	const auto* Subgraph = Cast<UPCGSubgraphSettings>(Settings);
	if (!Subgraph) return;
	auto Snapshot = MakeShared<FJsonObject>();
	Snapshot->SetStringField(TEXT("path"), MCPSubgraphPathOf(Subgraph));
	auto Overrides = MakeShared<FJsonObject>();
	for (const auto& Value : MCPDescribeSubgraphParameters(const_cast<UPCGSubgraphSettings*>(Subgraph)))
	{
		const auto Param = Value->AsObject();
		if (Param->GetBoolField(TEXT("overridden"))) Overrides->SetField(Param->GetStringField(TEXT("name")), Param->TryGetField(TEXT("value")));
	}
	Snapshot->SetObjectField(TEXT("parameters"), Overrides);
	Node->SetObjectField(TEXT("subgraph"), Snapshot);
}

bool FPCGHandlers::ImportSubgraphSettings(UPCGSettings* Settings, const TSharedPtr<FJsonObject>& Node, FString& Error)
{
	if (!Node->HasField(TEXT("subgraph"))) return true;
	const TSharedPtr<FJsonObject>* Snapshot = nullptr;
	FString Path;
	const TSharedPtr<FJsonObject>* Overrides = nullptr;
	auto* Subgraph = Cast<UPCGSubgraphSettings>(Settings);
	if (!Subgraph || !Node->TryGetObjectField(TEXT("subgraph"), Snapshot) || !Snapshot
		|| !(*Snapshot)->TryGetStringField(TEXT("path"), Path)
		|| !(*Snapshot)->TryGetObjectField(TEXT("parameters"), Overrides) || !Overrides)
	{
		Error = TEXT("subgraph requires a Subgraph node and {path, parameters} snapshot");
		return false;
	}
	UPCGGraphInterface* Graph = Path.IsEmpty() ? nullptr : LoadAssetByPath<UPCGGraphInterface>(Path);
	if (!Path.IsEmpty() && !Graph)
	{
		Error = FString::Printf(TEXT("subgraph asset could not be loaded: %s"), *Path);
		return false;
	}
	Subgraph->SetSubgraph(Graph);
	FMCPSubgraphNode Target{Settings->GetTypedOuter<UPCGGraph>(), Settings->GetTypedOuter<UPCGNode>(), Subgraph};
	Target.Node->UpdateAfterSettingsChangeDuringCreation();
	if ((*Overrides)->Values.Num() == 0) return true;
	TSharedPtr<FJsonObject> Previous;
	return MCPApplySubgraphParameters(Target, *Overrides, Previous, Error);
}
