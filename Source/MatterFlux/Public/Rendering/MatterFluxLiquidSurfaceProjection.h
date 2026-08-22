#pragma once

#include "CoreMinimal.h"
#include "Material/MatterFluxMaterialWorld.h"

namespace MatterFlux::Rendering
{
	/**
	 * Render-only projection of a canonical liquid body. The material cells are
	 * facts; this structure is disposable geometry derived from their connected
	 * shape. It is never fed back into the falling-sand simulation.
	 */
	struct MATTERFLUX_API FLiquidSurfaceProjection
	{
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TMap<FIntPoint, float> SurfaceHeights;
		/** Number of leading triangle indices belonging to upward free surfaces. */
		int32 TopTriangleIndexCount = 0;
		int32 SurfacePatchCount = 0;
		int32 ProjectedCellCount = 0;
		float CanonicalMedianSurfaceHeight = 0.0f;
		float ProjectedCanonicalMedianSurfaceHeight = 0.0f;
		float MedianCanonicalHeightOffset = 0.0f;
		float MaximumAbsoluteCanonicalHeightOffset = 0.0f;

		void Reset();
	};

	/**
	 * Builds disposable surface geometry from the current canonical snapshot only.
	 * A waterfall can belong to one material body without forcing its upper and
	 * lower surfaces into one stretched sheet. Adjacent cells share a corner whose
	 * height interpolates only those current cells. The projection never fills an
	 * empty coordinate or changes a current column height; no temporal or body-wide
	 * height is allowed to exist independently of material facts.
	 */
	MATTERFLUX_API void BuildLiquidSurfaceProjection(
		TConstArrayView<Material::FCellSnapshot> Cells,
		float CellSize,
		float FullColumnHeight,
		FLiquidSurfaceProjection& OutProjection);

	/**
	 * Builds only the top/side faces owned by one render chunk. Cells may include
	 * a one-cell halo; halo facts influence shared corners and exposed-edge tests
	 * but never emit geometry into this chunk's mesh.
	 */
	MATTERFLUX_API void BuildLiquidSurfaceChunkProjection(
		TConstArrayView<Material::FCellSnapshot> CellsWithHalo,
		float CellSize,
		float FullColumnHeight,
		FIntPoint ChunkCoordinate,
		int32 ChunkSize,
		FLiquidSurfaceProjection& OutProjection);

	/** Stable two-colour partition used by the projection task scheduler. */
	MATTERFLUX_API void PartitionLiquidProjectionChunksCheckerboard(
		TConstArrayView<FIntPoint> Chunks,
		TArray<FIntPoint>& OutEvenChunks,
		TArray<FIntPoint>& OutOddChunks);
}
