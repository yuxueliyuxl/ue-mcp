#if WITH_DEV_AUTOMATION_TESTS

#include "HandlerRegistry.h"
#include "Handlers/Material/MaterialHandlers.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaterialExpressionLookupByDescTest,
	"UE.MCP.Material.ExpressionLookup.ByDesc",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// add_expression stores its `name` in Desc, and a Constant's GetDescription is
// its value, so a name lookup that skips Desc cannot find what it just created.
bool FMaterialExpressionLookupByDescTest::RunTest(const FString& Parameters)
{
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/MCPLookup_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TStrongObjectPtr<UMaterial> Material(NewObject<UMaterial>(Package, TEXT("Material"), RF_Transient));
	auto* Metallic = NewObject<UMaterialExpressionConstant>(Material.Get(), NAME_None, RF_Transient);
	auto* Roughness = NewObject<UMaterialExpressionConstant>(Material.Get(), NAME_None, RF_Transient);
	Metallic->Desc = TEXT("Metallic");
	Metallic->R = 1.0f;
	Roughness->Desc = TEXT("Roughness");
	Roughness->R = 0.3f;
	Material->GetExpressionCollection().AddExpression(Metallic);
	Material->GetExpressionCollection().AddExpression(Roughness);

	FMCPHandlerRegistry Registry;
	FMaterialHandlers::RegisterHandlers(Registry);
	auto Connect = [&](const TCHAR* Name, const TCHAR* Property) -> bool
	{
		auto Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("materialPath"), Material->GetPathName());
		Request->SetStringField(TEXT("expressionName"), Name);
		Request->SetStringField(TEXT("property"), Property);
		const auto Value = Registry.ExecuteHandler(TEXT("connect_to_material_property"), Request);
		return Value.IsValid() && Value->Type == EJson::Object && Value->AsObject()->GetBoolField(TEXT("success"));
	};

	TestTrue(TEXT("Metallic connects by its name"), Connect(TEXT("Metallic"), TEXT("Metallic")));
	TestTrue(TEXT("Roughness connects by its name"), Connect(TEXT("Roughness"), TEXT("Roughness")));
	TestTrue(TEXT("Metallic input holds the node named Metallic"), Material->GetExpressionInputForProperty(MP_Metallic)->Expression == Metallic);
	TestTrue(TEXT("Roughness input holds the node named Roughness"), Material->GetExpressionInputForProperty(MP_Roughness)->Expression == Roughness);
	TestFalse(TEXT("an unknown name is refused"), Connect(TEXT("Specular"), TEXT("Specular")));
	return true;
}

#endif
