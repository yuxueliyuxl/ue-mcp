// disconnect_pcg_nodes must tell the graph its structure changed; without that the compiled graph and
// cached results keep the removed edge until something else invalidates them.
#if WITH_DEV_AUTOMATION_TESTS

#include "HandlerRegistry.h"
#include "Handlers/PCG/PCGHandlers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Elements/PCGAddTag.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "UObject/Package.h"
#include "Tests/MCPScopedTestMount.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPPCGDisconnectNotifiesTest,
	"UE.MCP.PCG.Disconnect.NotifiesStructureChange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPPCGDisconnectNotifiesTest::RunTest(const FString& Parameters)
{
	FMCPScopedTestMount Mount(
		TEXT("/UEMCPPCGDisconnect_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/"), TEXT("UEMCPPCGDisconnect"));
	const FString PackageName = Mount.RootPath + TEXT("PG_DisconnectProbe");

	UPackage* Package = CreatePackage(*PackageName);
	UPCGGraph* Graph = NewObject<UPCGGraph>(Package, TEXT("PG_DisconnectProbe"), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("PCG graph fixture created"), Graph)) return false;

	UPCGAddTagSettings* FromSettings = nullptr;
	UPCGAddTagSettings* ToSettings = nullptr;
	UPCGNode* From = Graph->AddNodeOfType(FromSettings);
	UPCGNode* To = Graph->AddNodeOfType(ToSettings);
	if (!TestTrue(TEXT("two nodes added"), From && To)) return false;
	if (!TestNotNull(TEXT("edge added"), Graph->AddEdge(From, PCGPinConstants::DefaultOutputLabel, To, PCGPinConstants::DefaultInputLabel))) return false;

	int32 StructuralEdgeChanges = 0;
	const FDelegateHandle Handle = Graph->OnGraphChangedDelegate.AddLambda([&StructuralEdgeChanges](UPCGGraphInterface*, EPCGChangeType ChangeType)
	{
		if (EnumHasAnyFlags(ChangeType, EPCGChangeType::Structural | EPCGChangeType::Edge)) ++StructuralEdgeChanges;
	});

	FMCPHandlerRegistry Registry;
	FPCGHandlers::RegisterHandlers(Registry);
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("assetPath"), PackageName);
	Params->SetStringField(TEXT("sourceNode"), From->GetName());
	Params->SetStringField(TEXT("targetNode"), To->GetName());
	const TSharedPtr<FJsonValue> Value = Registry.ExecuteHandler(TEXT("disconnect_pcg_nodes"), Params);
	Graph->OnGraphChangedDelegate.Remove(Handle);

	const TSharedPtr<FJsonObject> Result = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
	double Removed = 0;
	if (!TestTrue(TEXT("disconnect_pcg_nodes reports one removed edge"), Result.IsValid() && Result->TryGetNumberField(TEXT("removedEdges"), Removed) && Removed == 1)) return false;

	const UPCGPin* OutPin = From->GetOutputPin(PCGPinConstants::DefaultOutputLabel);
	const UPCGPin* InPin = To->GetInputPin(PCGPinConstants::DefaultInputLabel);
	TestTrue(TEXT("the source pin has no edges"), OutPin && OutPin->Edges.Num() == 0);
	TestTrue(TEXT("the target pin has no edges"), InPin && InPin->Edges.Num() == 0);
	TestTrue(TEXT("the graph broadcast a structural or edge change"), StructuralEdgeChanges > 0);
	return true;
}

#endif
