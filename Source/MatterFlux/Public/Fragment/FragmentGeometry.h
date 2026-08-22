#pragma once

#include "CoreMinimal.h"
#include "Fragment/FragmentTypes.h"

namespace MatterFlux::FragmentGeometry
{
	inline constexpr int32 MaximumFragmentCount = 16;
	inline constexpr int32 MaximumReplicatedFaceVertices = 768;
	inline constexpr int32 MaximumReplicatedTriangleIndices = 1536;
	inline constexpr int32 MaximumReplicatedContours = 256;
	inline constexpr int32 MaximumReplicatedVerticesPerContour = 768;
	inline constexpr int32 MaximumReplicatedContourVertices = 1536;

	struct FFragmentComponent
	{
		TArray<FIntPoint> Cells;
		FIntPoint Min = FIntPoint(MAX_int32, MAX_int32);
		FIntPoint Max = FIntPoint(MIN_int32, MIN_int32);
	};

	struct MATTERFLUX_API FFragmentGeometry2D
	{
		TArray<FVector2D> Vertices2D;
		TArray<int32> TriangleIndices;
		TArray<FFragmentContour> OuterContours;
		TArray<FFragmentContour> HoleContours;
		TArray<FFragmentContour> CollisionContours;
	};

	struct MATTERFLUX_API FFragmentSupportResult
	{
		TArray<uint8> SupportedMask;
		TArray<FFragmentComponent> DetachedComponents;
	};

	MATTERFLUX_API bool IsSolid(const TArray<uint8>& Mask, int32 Width, int32 Height, int32 X, int32 Y);
	MATTERFLUX_API bool ApplyDamageShape(TArray<uint8>& Mask, int32 Width, int32 Height, float CellSize, const FFragmentDamageShape& LocalDamageShape);
	MATTERFLUX_API void ExtractConnectedComponents(const TArray<uint8>& Mask, int32 Width, int32 Height, TArray<FFragmentComponent>& OutComponents);
	MATTERFLUX_API bool BuildSupportAnchorMask(
		const TArray<uint8>& InitialMask,
		int32 Width,
		int32 Height,
		EFragmentSupportMode SupportMode,
		TArray<uint8>& OutAnchorMask);
	MATTERFLUX_API bool ClassifyMaskBySupport(
		const TArray<uint8>& CandidateMask,
		const TArray<uint8>& AnchorMask,
		int32 Width,
		int32 Height,
		EFragmentSupportMode SupportMode,
		FFragmentSupportResult& OutResult);
	MATTERFLUX_API bool BuildFragmentGeometryFromMask(
		const TArray<uint8>& Mask,
		int32 Width,
		int32 Height,
		float CellSize,
		FFragmentGeometry2D& OutGeometry);
	MATTERFLUX_API bool IsSpawnPayloadWithinReplicationBudget(const FFragmentSpawnPayload& Payload);
	MATTERFLUX_API bool BuildSpawnPayloadsFromComponents(
		const TArray<FFragmentComponent>& Components,
		const FGuid& SourceId,
		const FTransform& SourceTransform,
		int32 MaskWidth,
		int32 MaskHeight,
		int32 Revision,
		float CellSize,
		int32 MinAreaPixels,
		int32 MaxFragments,
		const FVector& DamageCenterWorld,
		float DamagePower,
		int32 EventSeed,
		TArray<FFragmentSpawnPayload>& OutPayloads,
		EFragmentSourceGeometryStyle GeometryStyle =
			EFragmentSourceGeometryStyle::ExtrudedMask);
	MATTERFLUX_API bool BuildExtrudedMesh(
		const TArray<FVector2D>& Vertices2D,
		const TArray<int32>& TriangleIndices,
		const TArray<FFragmentContour>& OuterContours,
		const TArray<FFragmentContour>& HoleContours,
		float Thickness,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals,
		TArray<FVector2D>& OutUVs);
	/** Builds one slightly inset cube per solid cell while keeping one mesh batch. */
	MATTERFLUX_API bool BuildVoxelBlockMeshFromMask(
		const TArray<uint8>& Mask,
		int32 Width,
		int32 Height,
		float CellSize,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals,
		TArray<FVector2D>& OutUVs,
		int32& OutPrimaryIndexCount);
	/**
	 * Builds one watertight render mesh from a set of occupied 3D voxel cells.
	 * Faces shared by adjacent cells are removed, including depth-adjacent cells
	 * that originate from separate MatterFlux 2D source masks.
	 */
	MATTERFLUX_API bool BuildVoxelBlockMeshFromCells(
		const TArray<FIntVector>& SolidCells,
		float CellSize,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals,
		TArray<FVector2D>& OutUVs,
		int32& OutPrimaryIndexCount);
	/**
	 * Builds only SurfaceCells while treating OccluderCells as occupied neighbours.
	 * This lets independently editable material layers share one voxel surface
	 * without emitting hidden or coincident faces at their boundaries.
	 */
	MATTERFLUX_API bool BuildVoxelBlockMeshFromCellsWithOccluders(
		const TArray<FIntVector>& SurfaceCells,
		const TArray<FIntVector>& OccluderCells,
		float CellSize,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals,
		TArray<FVector2D>& OutUVs,
		int32& OutPrimaryIndexCount);
	/**
	 * Selects burning mask cells that are exposed on the top of a voxel column.
	 * The output uses stable mask-index order and is cleared on invalid input.
	 */
	MATTERFLUX_API bool GatherTopExposedBurningMaskCells(
		const TArray<uint8>& OccupiedMask,
		const TArray<uint8>& BurningMask,
		int32 Width,
		int32 Height,
		TArray<int32>& OutCellIndices);
	/** Builds a deterministic faceted cylinder stack from contiguous mask rows. */
	MATTERFLUX_API bool BuildRadialColumnMeshFromMask(
		const TArray<uint8>& Mask,
		int32 Width,
		int32 Height,
		float CellSize,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals,
		TArray<FVector2D>& OutUVs,
		int32& OutPrimaryIndexCount);
}
