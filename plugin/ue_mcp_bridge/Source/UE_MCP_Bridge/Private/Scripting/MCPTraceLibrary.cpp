#include "Scripting/MCPTraceLibrary.h"

#include "CollisionQueryParams.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HandlerObjectUtils.h"
#include "HandlerWorld.h"
#include "PhysicalMaterials/PhysicalMaterial.h"

FMCPTraceHit UMCPTraceLibrary::LineTrace(FVector Start, FVector End, const FString& World, int32 PieInstance,
	const FString& Channel, bool bTraceComplex)
{
	FMCPTraceHit Out;
	UWorld* TraceWorld = ResolveWorldScope(World, PieInstance);
	if (!TraceWorld)
	{
		Out.Error = FString::Printf(TEXT("No world for scope '%s'%s"), *World,
			World.Equals(TEXT("editor"), ESearchCase::IgnoreCase) ? TEXT("") : TEXT(": is PIE running?"));
		return Out;
	}
	Out.World = TraceWorld->GetPathName();

	ECollisionChannel TraceChannel = ECC_Visibility;
	FString ResolvedChannel;
	if (!MCPResolveCollisionChannel(Channel, TraceChannel, ResolvedChannel))
	{
		Out.Error = FString::Printf(TEXT("Unknown collision channel '%s'"), *Channel);
		return Out;
	}

	FCollisionQueryParams Query(SCENE_QUERY_STAT(MCPPythonLineTrace), bTraceComplex);
	Query.bReturnPhysicalMaterial = true;
	FHitResult Hit;
	Out.bHit = TraceWorld->LineTraceSingleByChannel(Hit, Start, End, TraceChannel, Query);
	if (!Out.bHit) return Out;

	Out.Location = Hit.Location;
	Out.ImpactPoint = Hit.ImpactPoint;
	Out.ImpactNormal = Hit.ImpactNormal;
	Out.Distance = Hit.Distance;
	if (AActor* HitActor = Hit.GetActor())
	{
		Out.ActorPath = HitActor->GetPathName();
		Out.ActorLabel = HitActor->GetActorLabel();
	}
	if (UPrimitiveComponent* HitComponent = Hit.GetComponent())
	{
		Out.ComponentName = HitComponent->GetName();
	}
	if (Hit.PhysMaterial.IsValid())
	{
		Out.PhysicalMaterial = Hit.PhysMaterial->GetPathName();
	}
	return Out;
}
