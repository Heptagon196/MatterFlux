#pragma once

#include "CoreMinimal.h"

class AFragment2DSourceActor;

namespace MatterFlux::MaterialCutaway
{
	/** Bounded policy compiled from Lua; material roles remain canonical facts. */
	struct MATTERFLUX_API FPolicy
	{
		float ContactToleranceCentimeters = 12.0f;
		float FloorSnapHeightCentimeters = 28.0f;
		float PreferredFloorPaddingCentimeters = 90.0f;
		float PreferredFloorVerticalRangeCentimeters = 420.0f;
		float OcclusionProbeRadiusCentimeters = 8.0f;
	};

	/** Disposable visibility projection derived from current Source facts. */
	struct MATTERFLUX_API FResult
	{
		FGuid FloorSourceId;
		float FloorSurfaceZ = 0.0f;
		int32 FloorOrdinal = INDEX_NONE;
		TSet<FGuid> GhostSourceIds;

		bool HasFloor() const { return FloorSourceId.IsValid(); }
	};

	/**
	 * Resolves the floor below ViewerFeet and the live wall Sources connected to
	 * that floor.  Actor ownership, class and display mesh are intentionally not
	 * part of the interface.
	 */
	MATTERFLUX_API bool Resolve(
		const FVector& ViewerFeet,
		TConstArrayView<AFragment2DSourceActor*> Sources,
		const FGuid& PreferredFloorSourceId,
		const FPolicy& Policy,
		FResult& OutResult);

	/**
	 * Resolves live wall material between CameraLocation and the viewer silhouette,
	 * then expands through connected wall and floor material within the supplied
	 * house. Furniture is never pulled into the structural fade graph.
	 */
	MATTERFLUX_API bool ResolveOccludingWalls(
		const FVector& CameraLocation,
		const FBox& ViewerBounds,
		TConstArrayView<AFragment2DSourceActor*> Sources,
		const FPolicy& Policy,
		FResult& OutResult);

	inline bool Resolve(
		const FVector& ViewerFeet,
		const TConstArrayView<AFragment2DSourceActor*> Sources,
		const FGuid& PreferredFloorSourceId,
		FResult& OutResult)
	{
		return Resolve(
			ViewerFeet, Sources, PreferredFloorSourceId, FPolicy(), OutResult);
	}
}
