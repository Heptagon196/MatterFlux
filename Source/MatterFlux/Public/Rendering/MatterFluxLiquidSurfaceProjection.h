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
		/** Canonical liquid-column depth, aligned one-to-one with Vertices. */
		TArray<float> ColumnDepths;
		TMap<FIntPoint, float> SurfaceHeights;
		/** Number of leading vertices belonging to upward free surfaces. */
		int32 TopVertexCount = 0;
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
	 * lower surfaces into one stretched sheet. Every material cell owns its four
	 * top vertices so interpolation cannot turn adjacent liquid particles into a
	 * continuous curved sheet. Different-height cells retain separate flat tops
	 * and an explicit stepped side. The projection never fills an empty coordinate
	 * or changes a current column height; no temporal or body-wide height is
	 * allowed to exist independently of material facts.
	 */
	MATTERFLUX_API void BuildLiquidSurfaceProjection(
		TConstArrayView<Material::FCellSnapshot> Cells,
		float CellSize,
		float FullColumnHeight,
		FLiquidSurfaceProjection& OutProjection);

	/**
	 * Builds only the top/side faces owned by one render chunk. Cells may include
	 * a one-cell halo; halo facts influence shared vertices and exposed-edge tests
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

	/**
	 * Selects the bounded projection work for one visualization pass. This is a
	 * render scheduler only; it never changes canonical material state.
	 */
	MATTERFLUX_API void SelectLiquidProjectionChunksForRebuild(
		TConstArrayView<FIntPoint> PendingChunks,
		FIntPoint FocusChunk,
		TArray<FIntPoint>& OutSelectedChunks,
		const TMap<FIntPoint, uint64>* EnqueueOrders = nullptr,
		int32 MaximumChunksPerPass = 8);
}
