#pragma once

#include "CoreMinimal.h"

namespace MatterFlux::ItemOcclusion
{
	/** One locally renderable material item. No Actor ownership leaks in here. */
	struct MATTERFLUX_API FItem
	{
		FGuid ItemId;
		FGuid ConnectionId;
		FBox WorldBounds = FBox(ForceInit);
		float CellSize = 1.0f;
	};

	struct MATTERFLUX_API FPolicy
	{
		/** A narrow probe catches partial silhouette obstruction without fading nearby scenery. */
		float ProbeRadiusCentimeters = 8.0f;
	};

	struct MATTERFLUX_API FResult
	{
		TSet<FGuid> GhostItemIds;

		bool IsEmpty() const { return GhostItemIds.IsEmpty(); }
	};

	/**
	 * Finds items between CameraLocation and the viewer silhouette, then includes
	 * every part with the same explicit connection identity. Bounds contact is not
	 * connectivity: nearby independent scenery must stay solid. The result is a
	 * local, disposable rendering projection and never mutates gameplay state.
	 */
	MATTERFLUX_API bool Resolve(
		const FVector& CameraLocation,
		const FBox& ViewerBounds,
		TConstArrayView<FItem> Items,
		const FPolicy& Policy,
		FResult& OutResult);

	inline bool Resolve(
		const FVector& CameraLocation,
		const FBox& ViewerBounds,
		const TConstArrayView<FItem> Items,
		FResult& OutResult)
	{
		return Resolve(
			CameraLocation, ViewerBounds, Items, FPolicy(), OutResult);
	}
}
