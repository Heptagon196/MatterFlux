#pragma once

#include "CoreMinimal.h"
#include "Game/MatterFluxPlayableLevel.h"
#include "Volume/MatterFluxMaterialVolume.h"

namespace MatterFlux::TerrainMesh
{
	struct MATTERFLUX_API FSection
	{
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		/** Resolved once on the game thread when the mesh section is submitted. */
		FLinearColor Color = FLinearColor::White;
		/** Zero for legacy terrain-band sections; otherwise the stable material index. */
		uint16 MaterialIndex = 0;

		bool IsValid() const;
	};

	struct MATTERFLUX_API FChunk
	{
		FIntPoint ChunkCoordinate = FIntPoint::ZeroValue;
		FIntRect CellBounds;
		TArray<FSection> Sections;
		FSection CollisionSurface;
		/** Heightfield chunks stay cheap; only edited/hollow chunks use the volume path. */
		bool bUsesVolumeSurface = false;

		bool IsValid() const;
		int32 GetTriangleCount() const;
	};

	/**
	 * Immutable, UObject-free facts captured before dispatching a terrain mesh task.
	 * The procedural baseline remains implicit; only edited columns are copied.
	 */
	struct MATTERFLUX_API FVolumeSnapshot
	{
		TMap<FIntPoint, TArray<FMaterialSpan>> ColumnOverrides;
		TMap<uint16, FLinearColor> MaterialColors;
		uint16 SoilMaterialIndex = 0;
		uint16 SurfaceMaterialIndex = 0;

		bool IsValid(FString* OutError = nullptr) const;
		bool HasVolumeFactsNearChunk(FIntPoint ChunkCoordinate, int32 ChunkSize) const;
	};

	struct MATTERFLUX_API FSurfaceHit
	{
		FMaterialSurfaceKey Surface;
		uint16 MaterialIndex = 0;
		FVector LocalLocation = FVector::ZeroVector;
		FVector LocalNormal = FVector::UpVector;
		double Time = 1.0;
	};

	MATTERFLUX_API bool BuildChunk(
		const PlayableLevel::FLevelTerrain& Terrain,
		FIntPoint ChunkCoordinate,
		int32 ChunkSize,
		FChunk& OutChunk);

	MATTERFLUX_API bool BuildChunk(
		const PlayableLevel::FLevelTerrain& Terrain,
		const FVolumeSnapshot& Volume,
		FIntPoint ChunkCoordinate,
		int32 ChunkSize,
		FChunk& OutChunk);

	/** Sweeps a sphere only against exposed faces in the sparse volume region. */
	MATTERFLUX_API bool SweepVolumeSurface(
		const PlayableLevel::FLevelTerrain& Terrain,
		const FVolumeSnapshot& Volume,
		const FVector& LocalStart,
		const FVector& LocalEnd,
		float Radius,
		FSurfaceHit& OutHit);
}
