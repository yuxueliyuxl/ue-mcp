#include "LevelHandlers.h"

#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "HandlerUtils.h"
#include "Misc/AutomationTest.h"
#include "ScopedTransaction.h"

namespace
{
	USceneComponent* FindExactSceneComponent(AActor* Actor, const FString& ComponentName)
	{
		if (!Actor || ComponentName.IsEmpty()) return nullptr;
		for (UActorComponent* Component : Actor->GetComponents())
		{
			if (USceneComponent* Scene = Cast<USceneComponent>(Component);
				Scene && Scene->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
			{
				return Scene;
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> QuatToJson(const FQuat& Quat)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("x"), Quat.X);
		Json->SetNumberField(TEXT("y"), Quat.Y);
		Json->SetNumberField(TEXT("z"), Quat.Z);
		Json->SetNumberField(TEXT("w"), Quat.W);
		return Json;
	}

	TSharedPtr<FJsonObject> TransformToJson(const FTransform& Transform)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetObjectField(TEXT("location"), MCPVec3ToJsonObject(Transform.GetLocation()));
		Json->SetObjectField(TEXT("rotation"), MCPRotatorToJsonObject(Transform.Rotator()));
		Json->SetObjectField(TEXT("quaternion"), QuatToJson(Transform.GetRotation()));
		Json->SetObjectField(TEXT("scale"), MCPVec3ToJsonObject(Transform.GetScale3D()));
		return Json;
	}

	TSharedPtr<FJsonObject> ComponentTransformsToJson(USceneComponent* Component)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetObjectField(TEXT("relative"), TransformToJson(Component->GetRelativeTransform()));
		Json->SetObjectField(TEXT("world"), TransformToJson(Component->GetComponentTransform()));
		return Json;
	}

	bool ReadRequiredNumber(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field, double& Out)
	{
		return Json && Json->TryGetNumberField(Field, Out) && FMath::IsFinite(Out);
	}

	bool ReadRollbackTransform(const TSharedPtr<FJsonObject>& Json, FTransform& Out)
	{
		if (!Json) return false;

		const TSharedPtr<FJsonObject>* Location = nullptr;
		const TSharedPtr<FJsonObject>* Quaternion = nullptr;
		const TSharedPtr<FJsonObject>* Scale = nullptr;
		if (!Json->TryGetObjectField(TEXT("location"), Location) || !*Location ||
			!Json->TryGetObjectField(TEXT("quaternion"), Quaternion) || !*Quaternion ||
			!Json->TryGetObjectField(TEXT("scale"), Scale) || !*Scale)
		{
			return false;
		}

		double LX, LY, LZ, QX, QY, QZ, QW, SX, SY, SZ;
		if (!ReadRequiredNumber(*Location, TEXT("x"), LX) ||
			!ReadRequiredNumber(*Location, TEXT("y"), LY) ||
			!ReadRequiredNumber(*Location, TEXT("z"), LZ) ||
			!ReadRequiredNumber(*Quaternion, TEXT("x"), QX) ||
			!ReadRequiredNumber(*Quaternion, TEXT("y"), QY) ||
			!ReadRequiredNumber(*Quaternion, TEXT("z"), QZ) ||
			!ReadRequiredNumber(*Quaternion, TEXT("w"), QW) ||
			!ReadRequiredNumber(*Scale, TEXT("x"), SX) ||
			!ReadRequiredNumber(*Scale, TEXT("y"), SY) ||
			!ReadRequiredNumber(*Scale, TEXT("z"), SZ))
		{
			return false;
		}

		FQuat Rotation(QX, QY, QZ, QW);
		if (Rotation.SizeSquared() <= UE_SMALL_NUMBER) return false;
		Rotation.Normalize();
		Out = FTransform(Rotation, FVector(LX, LY, LZ), FVector(SX, SY, SZ));
		return true;
	}

	FVector NamedAxis(const FString& Axis)
	{
		if (Axis == TEXT("forward")) return FVector::ForwardVector;
		if (Axis == TEXT("right")) return FVector::RightVector;
		return FVector::UpVector;
	}

	FTransform ApplyWorldNudge(
		const FTransform& Current,
		const FVector& TranslationWorld,
		bool bRotate,
		const FVector& RotationAxisWorld,
		double RotationDegrees)
	{
		FTransform Result = Current;
		Result.AddToTranslation(TranslationWorld);
		if (bRotate)
		{
			FQuat NewRotation = FQuat(
				RotationAxisWorld.GetSafeNormal(),
				FMath::DegreesToRadians(RotationDegrees)) * Current.GetRotation();
			NewRotation.Normalize();
			Result.SetRotation(NewRotation);
		}
		return Result;
	}
}

TSharedPtr<FJsonValue> FLevelHandlers::NudgeComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (auto Error = RequireStringAlt(Params, TEXT("actorLabel"), TEXT("actorPath"), ActorLabel)) return Error;
	FString ComponentName;
	if (auto Error = RequireString(Params, TEXT("componentName"), ComponentName)) return Error;

	const FString WorldScope = OptionalString(Params, TEXT("world"), TEXT("editor")).ToLower();
	UWorld* World = ResolveWorldFromParams(Params, *WorldScope);
	if (!World)
	{
		return MCPError(WorldScope == TEXT("pie")
			? TEXT("PIE not running (or no such pieInstance). See editor(list_pie_instances).")
			: TEXT("Editor world not available"));
	}

	FMCPActorSelector ActorSel;
	ActorSel.Match = EMCPActorMatch::LabelNameOrPath;
	ActorSel.WorldLabel = World->IsGameWorld() ? TEXT("PIE") : TEXT("editor");
	TSharedPtr<FJsonValue> ActorErr;
	AActor* Actor = MCPResolveActor(World, Params, ActorErr, ActorSel);
	if (!Actor) return ActorErr;
	ActorLabel = Actor->GetActorLabel();
	USceneComponent* Component = FindExactSceneComponent(Actor, ComponentName);
	if (!Component)
	{
		return MCPError(FString::Printf(
			TEXT("Exact SceneComponent '%s' not found on actor '%s'"), *ComponentName, *ActorLabel));
	}

	const FTransform PreviousRelative = Component->GetRelativeTransform();
	const FTransform PreviousWorld = Component->GetComponentTransform();
	const TSharedPtr<FJsonObject>* RestoreJson = nullptr;
	const bool bRestore = Params->TryGetObjectField(TEXT("_restoreRelative"), RestoreJson) && *RestoreJson;

	FString Frame = OptionalString(Params, TEXT("frame"), TEXT("actor")).ToLower();
	FVector TranslationWorld = FVector::ZeroVector;
	FVector RotationAxisWorld = FVector::ZeroVector;
	FString RotationAxis;
	double RotationDegrees = 0.0;
	double ScaleMultiplier = 1.0;
	bool bHasTranslation = false;
	bool bHasRotation = false;
	bool bHasScale = false;
	FTransform RestoreRelative;

	if (bRestore)
	{
		if (!ReadRollbackTransform(*RestoreJson, RestoreRelative))
		{
			return MCPError(TEXT("Invalid internal rollback transform"));
		}
	}
	else
	{
		if (Frame != TEXT("world") && Frame != TEXT("actor") &&
			Frame != TEXT("parent") && Frame != TEXT("component"))
		{
			return MCPError(TEXT("frame must be world, actor, parent, or component"));
		}

		FQuat FrameRotation = FQuat::Identity;
		if (Frame == TEXT("actor"))
		{
			FrameRotation = Actor->GetActorQuat();
		}
		else if (Frame == TEXT("parent"))
		{
			USceneComponent* Parent = Component->GetAttachParent();
			if (!Parent) return MCPError(TEXT("frame=parent requires an attached component"));
			FrameRotation = Parent->GetComponentQuat();
		}
		else if (Frame == TEXT("component"))
		{
			FrameRotation = Component->GetComponentQuat();
		}
		FrameRotation.Normalize();

		const TSharedPtr<FJsonObject>* TranslationJson = nullptr;
		if (Params->TryGetObjectField(TEXT("translationDelta"), TranslationJson) && *TranslationJson)
		{
			double Forward = 0.0, Right = 0.0, Up = 0.0;
			if (((*TranslationJson)->HasField(TEXT("forwardCm")) &&
				 !ReadRequiredNumber(*TranslationJson, TEXT("forwardCm"), Forward)) ||
				((*TranslationJson)->HasField(TEXT("rightCm")) &&
				 !ReadRequiredNumber(*TranslationJson, TEXT("rightCm"), Right)) ||
				((*TranslationJson)->HasField(TEXT("upCm")) &&
				 !ReadRequiredNumber(*TranslationJson, TEXT("upCm"), Up)))
			{
				return MCPError(TEXT("translationDelta values must be finite numbers"));
			}
			TranslationWorld = FrameRotation.RotateVector(FVector(Forward, Right, Up));
			bHasTranslation = !TranslationWorld.IsNearlyZero();
		}

		const TSharedPtr<FJsonObject>* RotationJson = nullptr;
		if (Params->TryGetObjectField(TEXT("axisRotation"), RotationJson) && *RotationJson)
		{
			if (!(*RotationJson)->TryGetStringField(TEXT("axis"), RotationAxis) ||
				(RotationAxis != TEXT("forward") && RotationAxis != TEXT("right") && RotationAxis != TEXT("up")))
			{
				return MCPError(TEXT("axisRotation.axis must be forward, right, or up"));
			}
			if (!ReadRequiredNumber(*RotationJson, TEXT("degrees"), RotationDegrees))
			{
				return MCPError(TEXT("axisRotation.degrees must be a finite number"));
			}
			RotationAxisWorld = FrameRotation.RotateVector(NamedAxis(RotationAxis)).GetSafeNormal();
			bHasRotation = !FMath::IsNearlyZero(RotationDegrees);
		}

		if (Params->HasField(TEXT("scaleMultiplier")))
		{
			if (!Params->TryGetNumberField(TEXT("scaleMultiplier"), ScaleMultiplier) ||
				!FMath::IsFinite(ScaleMultiplier) || ScaleMultiplier <= 0.0)
			{
				return MCPError(TEXT("scaleMultiplier must be a finite number greater than zero"));
			}
			bHasScale = !FMath::IsNearlyEqual(ScaleMultiplier, 1.0);
		}

		if (!bHasTranslation && !bHasRotation && !bHasScale)
		{
			return MCPError(TEXT("Provide a non-zero translationDelta, axisRotation, or scaleMultiplier"));
		}
	}

	const bool bRuntimeWorld = World->IsGameWorld();
	const FScopedTransaction Transaction(FText::FromString(TEXT("Nudge component")), !bRuntimeWorld);
	if (!bRuntimeWorld)
	{
		Actor->Modify();
		Component->Modify();
	}

	if (bRestore)
	{
		Component->SetRelativeTransform(RestoreRelative);
	}
	else
	{
		const FTransform AdjustedWorld = ApplyWorldNudge(
			PreviousWorld, TranslationWorld, bHasRotation, RotationAxisWorld, RotationDegrees);
		Component->SetWorldLocationAndRotation(
			AdjustedWorld.GetLocation(), AdjustedWorld.GetRotation(), false, nullptr, ETeleportType::TeleportPhysics);
		if (bHasScale)
		{
			Component->SetRelativeScale3D(PreviousRelative.GetScale3D() * ScaleMultiplier);
		}
	}

	Component->UpdateComponentToWorld();
	Component->MarkRenderStateDirty();
	if (!bRuntimeWorld)
	{
		Component->PostEditComponentMove(true);
		Component->MarkPackageDirty();
	}

	auto Result = MCPSuccess();
	MCPSetUpdated(Result);
	Result->SetStringField(TEXT("actorLabel"), ActorLabel);
	Result->SetStringField(TEXT("actorPath"), Actor->GetPathName());
	Result->SetStringField(TEXT("componentName"), Component->GetName());
	Result->SetStringField(TEXT("frame"), bRestore ? TEXT("rollback") : Frame);
	Result->SetStringField(TEXT("world"), WorldScope);
	Result->SetObjectField(TEXT("before"), [&]()
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetObjectField(TEXT("relative"), TransformToJson(PreviousRelative));
		Json->SetObjectField(TEXT("world"), TransformToJson(PreviousWorld));
		return Json;
	}());
	Result->SetObjectField(TEXT("after"), ComponentTransformsToJson(Component));
	if (bHasTranslation) Result->SetObjectField(TEXT("resolvedTranslationWorld"), MCPVec3ToJsonObject(TranslationWorld));
	if (bHasRotation)
	{
		Result->SetStringField(TEXT("rotationAxis"), RotationAxis);
		Result->SetNumberField(TEXT("rotationDegrees"), RotationDegrees);
		Result->SetObjectField(TEXT("resolvedRotationAxisWorld"), MCPVec3ToJsonObject(RotationAxisWorld));
	}
	if (bHasScale) Result->SetNumberField(TEXT("scaleMultiplier"), ScaleMultiplier);

	auto RollbackPayload = MakeShared<FJsonObject>();
	RollbackPayload->SetStringField(TEXT("actorLabel"), ActorLabel);
	RollbackPayload->SetStringField(TEXT("actorPath"), Actor->GetPathName());
	RollbackPayload->SetStringField(TEXT("componentName"), Component->GetName());
	RollbackPayload->SetStringField(TEXT("world"), WorldScope);
	double PieInstance = 0.0;
	if (Params->TryGetNumberField(TEXT("pieInstance"), PieInstance))
	{
		RollbackPayload->SetNumberField(TEXT("pieInstance"), PieInstance);
	}
	RollbackPayload->SetObjectField(TEXT("_restoreRelative"), TransformToJson(PreviousRelative));
	MCPSetRollback(Result, TEXT("nudge_component"), RollbackPayload);
	return MCPResult(Result);
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLevelSpatialComponentNudgeTest,
	"UE.MCP.Level.SpatialComponentNudge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLevelSpatialComponentNudgeTest::RunTest(const FString& Parameters)
{
	const FTransform ParentWorld(
		FRotator(18.0, -42.0, 27.0), FVector(400.0, -80.0, 120.0), FVector::OneVector);
	const FTransform Relative(
		FRotator(-147.657698, 83.868340, -89.500431), FVector(-9.0, 3.0, 0.0), FVector(1.35));
	const FTransform ChildWorld = Relative * ParentWorld;
	const FVector AxisWorld = FVector::UpVector;
	const FQuat ExpectedRotation =
		FQuat(AxisWorld, FMath::DegreesToRadians(12.0)) * ChildWorld.GetRotation();
	const FTransform Adjusted = ApplyWorldNudge(
		ChildWorld, FVector::ZeroVector, true, AxisWorld, 12.0);

	TestTrue(TEXT("Axis-angle nudge applies the requested world rotation"),
		Adjusted.GetRotation().AngularDistance(ExpectedRotation) < 1.e-6);
	TestTrue(TEXT("Rotation preserves world location"),
		Adjusted.GetLocation().Equals(ChildWorld.GetLocation(), 1.e-6));
	TestTrue(TEXT("Rotation preserves scale"),
		Adjusted.GetScale3D().Equals(ChildWorld.GetScale3D(), 1.e-6));

	const FTransform SolvedRelative = Adjusted.GetRelativeTransform(ParentWorld);
	const FTransform RoundTripWorld = SolvedRelative * ParentWorld;
	TestTrue(TEXT("Attached relative transform round-trips to the desired world rotation"),
		RoundTripWorld.GetRotation().AngularDistance(Adjusted.GetRotation()) < 1.e-6);

	FRotator NaiveRelativeRotation = Relative.Rotator();
	NaiveRelativeRotation.Yaw += 12.0;
	const FTransform NaiveRelative(
		NaiveRelativeRotation, Relative.GetLocation(), Relative.GetScale3D());
	const FTransform NaiveWorld = NaiveRelative * ParentWorld;
	TestTrue(TEXT("Raw relative Euler yaw is not the requested world-axis adjustment"),
		FMath::RadiansToDegrees(NaiveWorld.GetRotation().AngularDistance(Adjusted.GetRotation())) > 1.0);
	return true;
}
#endif
