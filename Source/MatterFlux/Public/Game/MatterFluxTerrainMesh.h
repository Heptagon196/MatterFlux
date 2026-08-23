#pragma once

#include "CoreMinimal.h"
#include "Game/MatterFluxPlayableLevel.h"

namespace MatterFlux::TerrainMesh
{
	struct MATTERFLUX_API FSection
	{
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;

		bool IsValid() const;
	};

	struct MATTERFLUX_API FChunk
	{
		FIntPoint ChunkCoordinate = FIntPoint::ZeroValue;
		FIntRect CellBounds;
		TArray<FSection> Sections;
		FSection CollisionSurface;

		bool IsValid() const;
		int32 GetTriangleCount() const;
	};

	MATTERFLUX_API bool BuildChunk(
		const PlayableLevel::FLevelTerrain& Terrain,
		FIntPoint ChunkCoordinate,
		int32 ChunkSize,
		FChunk& OutChunk);
}
