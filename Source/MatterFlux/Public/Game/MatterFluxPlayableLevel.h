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
		CollisionOnly
	};

	struct MATTERFLUX_API FLevelLayer
	{
		FName Name = NAME_None;
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
		int32 Width = 0;
		int32 Height = 0;
		float CellSize = 0.0f;
		float BottomZ = -110.0f;
		FVector2D FirstCellCenter = FVector2D::ZeroVector;
		TArray<float> Heights;
		TArray<uint8> ColorBands;
		TArray<FLinearColor> BandColors;

		bool IsValid() const
		{
			const int64 CellCount =
				static_cast<int64>(Width)
				* static_cast<int64>(Height);
			return Width > 0
				&& Height > 0
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
	};

	struct MATTERFLUX_API FLevelLayout
	{
		FLevelTerrain Terrain;
		TArray<FLevelLayer> Layers;
		TArray<FLevelFragmentSource> FragmentSources;

		const FLevelLayer* FindLayer(FName LayerName) const;
	};

	MATTERFLUX_API bool BuildLevelLayout(
		int32 Seed,
		FLevelLayout& OutLayout,
		const FMatterFluxContentRegistry* Content = nullptr);
}
