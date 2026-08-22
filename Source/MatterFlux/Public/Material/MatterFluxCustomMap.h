#pragma once

#include "CoreMinimal.h"

struct FMatterFluxContentRegistry;

namespace MatterFlux::Material
{
	class FChunkedMaterialWorld;

	struct FCustomMapSceneBox
	{
		FName Id;
		FName MaterialId;
		FVector Center = FVector::ZeroVector;
		FVector Size = FVector::OneVector;
		bool bCollision = false;
	};

	struct FCustomMapSceneCamera
	{
		FName Id;
		FVector Location = FVector::ZeroVector;
		FVector Target = FVector::ZeroVector;
		float FieldOfViewDegrees = 42.0f;
	};

	/** 固定步倾倒夹具的厘米制容器配置。 */
	struct FCustomMapPourContainer
	{
		FName Id;
		FName ContainerMaterialId;
		FName LiquidMaterialId;
		FVector Center = FVector::ZeroVector;
		FIntVector InteriorSizeCells = FIntVector(7, 5, 5);
		int32 StartStep = 12;
		int32 TiltDurationSteps = 36;
		float TiltDegrees = 72.0f;
		int32 PourCellsPerStep = 8;
	};

	/** 调用者可直接交给 UE 表现适配器的厘米制水平游戏场景。 */
	struct FCustomMapScene
	{
		float CellSizeCentimeters = 28.0f;
		float MaterialDepthCells = 3.0f;
		FIntPoint MinimumCell = FIntPoint::ZeroValue;
		FIntPoint MaximumCellExclusive = FIntPoint::ZeroValue;
		TMap<FName, FVector> MarkerLocations;
		TArray<FCustomMapSceneBox> Boxes;
		TArray<FCustomMapSceneCamera> Cameras;
		TArray<FCustomMapPourContainer> PourContainers;
	};

	/**
	 * 从 Lua 内容注册表中的一张自定义地图构建确定性材料世界。
	 * 调用者只提供 map id 与 seed；边界、物质填充顺序和活动区预算均由
	 * 本模块验证和推导，游戏、自动化测试与截图工具共用这一条 seam。
	 */
	MATTERFLUX_API bool BuildCustomMap(
		FName MapId,
		const FMatterFluxContentRegistry& Registry,
		int32 Seed,
		FChunkedMaterialWorld& OutWorld,
		FCustomMapScene& OutScene,
		FString& OutError);
}
