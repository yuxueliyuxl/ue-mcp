#include "VolumeHelpers_Internal.h"

#include "Engine/World.h"
#include "Engine/Brush.h"
#include "Engine/Polys.h"
#include "Components/BrushComponent.h"
#include "Builders/CubeBuilder.h"
#include "BSPOps.h"
#include "Model.h"
#include "GameFramework/Volume.h"
#include "NavigationSystem.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"

namespace UEMCP
{
	void BuildVolumeAsCube(UWorld* World, AVolume* Volume, const FVector& HalfExtent)
	{
		if (!World || !Volume) return;

		Volume->PreEditChange(nullptr);

		const EObjectFlags ObjectFlags = Volume->GetFlags() & (RF_Transient | RF_Transactional);
		if (!Volume->Brush)
		{
			Volume->PolyFlags = 0;
			Volume->Brush = NewObject<UModel>(Volume, NAME_None, ObjectFlags);
			Volume->Brush->Initialize(nullptr, true);
		}
		if (!Volume->Brush->Polys)
		{
			Volume->Brush->Polys = NewObject<UPolys>(Volume->Brush, NAME_None, ObjectFlags);
		}
		if (UBrushComponent* BC = Volume->GetBrushComponent())
		{
			BC->Brush = Volume->Brush;
		}

		// The builder is saved with the volume, so it must live inside it:
		// a transient-package builder is lost on save and reload.
		UCubeBuilder* CubeBuilder = Cast<UCubeBuilder>(Volume->BrushBuilder);
		if (!CubeBuilder || CubeBuilder->GetOuter() != Volume)
		{
			CubeBuilder = NewObject<UCubeBuilder>(Volume, NAME_None, ObjectFlags);
		}
		CubeBuilder->X = HalfExtent.X * 2.0;
		CubeBuilder->Y = HalfExtent.Y * 2.0;
		CubeBuilder->Z = HalfExtent.Z * 2.0;
		Volume->BrushBuilder = CubeBuilder;
		CubeBuilder->Build(World, Volume);

		Volume->SetActorScale3D(FVector::OneVector);

		FBSPOps::csgPrepMovingBrush(Volume);

		Volume->PostEditChange();
		NotifyVolumeBoundsChanged(Volume);
	}

	void NotifyVolumeBoundsChanged(AVolume* Volume)
	{
		ANavMeshBoundsVolume* NavVolume = Cast<ANavMeshBoundsVolume>(Volume);
		if (!NavVolume) return;
		if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(NavVolume->GetWorld()))
		{
			NavSys->OnNavigationBoundsUpdated(NavVolume);
		}
	}
}
