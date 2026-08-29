#pragma once

#include "CoreMinimal.h"
#include "Fragment/FragmentTypes.h"
#include "MatterFluxContentTypes.h"

namespace MatterFlux::PlayableLevel
{
	inline constexpr int32 TerrainCellsX = 512;
	inline constexpr int32 TerrainCellsY = 384;
	inline constexpr float TerrainCellSize = 8.0f;

	enum class ELayerPrimitive : uint8
	{
		Cube,
		Sphere,
		Cylinder,
		Cone
	};

	enum class ELevelLayerRenderMode : uint8
	{
		Lit,
		VoxelLit,
		VoxelUnlit,
		Liquid,
		CollisionOnly
	};

	struct MATTERFLUX_API FLevelLayer
	{
		FName Name = NAME_None;
		/** 可选的 Lua 材质 ID；液体层用它读取透明度参数。 */
		FName MaterialId = NAME_None;
		ELayerPrimitive Primitive = ELayerPrimitive::Cube;
		ELevelLayerRenderMode RenderMode = ELevelLayerRenderMode::Lit;
		FLinearColor Color = FLinearColor::White;
		bool bEnableCollision = false;
		TArray<FTransform> Instances;
	};

	struct MATTERFLUX_API FLevelFragmentSource
	{
		FName Name = NAME_None;
		FName MaterialId = NAME_None;
		FGuid SourceId;
		FGuid AggregateId;
		bool bAggregateRoot = false;
		FLinearColor Color = FLinearColor::White;
		bool bEnableCollision = false;
		FTransform Transform = FTransform::Identity;
		FFragmentSourceMask Mask;
	};

	struct MATTERFLUX_API FLevelTerrain
	{
		int32 Seed = 0;
		bool bInfinite = false;
		int32 Width = 0;
		int32 Height = 0;
		float CellSize = 0.0f;
		float BottomZ = -110.0f;
		FVector2D FirstCellCenter = FVector2D::ZeroVector;
		TArray<float> Heights;
		TArray<uint8> ColorBands;
		TArray<FLinearColor> BandColors;
		/**
		 * Sparse authoritative edits keyed by global terrain cell. Generated
		 * heights are immutable inputs; every cut, save, replicated client and
		 * streamed mesh samples this same runtime fact overlay.
		 */
		TMap<FIntPoint, float> RuntimeHeightOverrides;

		bool IsValid() const
		{
			const int64 CellCount =
				static_cast<int64>(Width)
				* static_cast<int64>(Height);
			return Width > 0
				&& Height > 0
				&& (!bInfinite || Seed != 0)
				&& CellCount > 0
				&& CellCount <= MAX_int32
				&& FMath::IsFinite(CellSize)
				&& CellSize > 0.0f
				&& FMath::IsFinite(BottomZ)
				&& FMath::IsFinite(FirstCellCenter.X)
				&& FMath::IsFinite(FirstCellCenter.Y)
				&& static_cast<int64>(Heights.Num()) == CellCount
				&& ColorBands.Num() == Heights.Num()
				&& BandColors.Num() == 3;
		}

		int32 ToIndex(const int32 X, const int32 Y) const
		{
			return Y * Width + X;
		}

		float HeightAt(const int32 X, const int32 Y) const
		{
			return Heights[ToIndex(X, Y)];
		}

		/**
		 * Samples the deterministic terrain lattice by global cell coordinate.
		 * Finite cached cells are returned verbatim; infinite terrain evaluates
		 * the same seeded noise function outside that cache.
		 */
		bool TrySampleWorldCell(
			int64 WorldCellX,
			int64 WorldCellY,
			float& OutHeight,
			uint8& OutColorBand) const;
		/** Samples only the immutable generated baseline, ignoring sparse edits. */
		bool TrySampleGeneratedWorldCell(
			int64 WorldCellX,
			int64 WorldCellY,
			float& OutHeight,
			uint8& OutColorBand) const;

		/** True when the global cell belongs to the eagerly generated seed area. */
		bool ContainsCachedWorldCell(int64 WorldCellX, int64 WorldCellY) const;

		/**
		 * Samples the deterministic river network used beyond the seed area.
		 * Returns true for both the wet channel and its sloped banks; callers can
		 * distinguish water from bank-only cells through bOutContainsWater.
		 */
		bool TrySampleInfiniteRiverCell(
			int64 WorldCellX,
			int64 WorldCellY,
			float& OutCarvedHeight,
			float& OutWaterSurface,
			bool& bOutContainsWater) const;
	};

	struct MATTERFLUX_API FStreamingRiverCell
	{
		FIntPoint WorldCell = FIntPoint::ZeroValue;
		float WaterSurfaceZ = 0.0f;
	};

	/** Deterministic content authored only for one resident terrain chunk. */
	struct MATTERFLUX_API FStreamingChunkPopulation
	{
		TArray<FLevelFragmentSource> FragmentSources;
		TArray<FStreamingRiverCell> RiverCells;
		bool bHasHouse = false;
		FVector HouseLocation = FVector::ZeroVector;
	};

	struct MATTERFLUX_API FLevelLayout
	{
		FLevelTerrain Terrain;
		/** 确定性的房屋预留位置；Z 在运行时按局部最高地形求得。 */
		FVector HouseLocation = FVector::ZeroVector;
		TArray<FLevelLayer> Layers;
		TArray<FLevelFragmentSource> FragmentSources;

		const FLevelLayer* FindLayer(FName LayerName) const;
	};

	MATTERFLUX_API bool BuildLevelLayout(
		int32 Seed,
		FLevelLayout& OutLayout,
		const FMatterFluxContentRegistry* Content = nullptr);

	MATTERFLUX_API bool BuildStreamingChunkPopulation(
		int32 Seed,
		const FLevelTerrain& Terrain,
		FIntPoint ChunkCoordinate,
		int32 ChunkSize,
		FStreamingChunkPopulation& OutPopulation,
		const FMatterFluxContentRegistry* Content = nullptr);
}
