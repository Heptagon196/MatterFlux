#pragma once

#include "CoreMinimal.h"
#include "Material/MatterFluxCustomMap.h"

struct FMatterFluxContentRegistry;

namespace MatterFlux::Material
{
	struct FCustomMapPourVoxel
	{
		FName MaterialId;
		FVector Position = FVector::ZeroVector;
		FVector Size = FVector::OneVector;
		FQuat Rotation = FQuat::Identity;
	};

	struct FCustomMapPourContainerSnapshot
	{
		FName Id;
		FName ContainerMaterialId;
		FName LiquidMaterialId;
		FTransform Transform = FTransform::Identity;
		FVector InteriorSize = FVector::ZeroVector;
		float WallThickness = 0.0f;
		float TiltDegrees = 0.0f;
		int32 InitialLiquidCells = 0;
		int32 HeldLiquidCells = 0;
	};

	struct FCustomMapPourSnapshot
	{
		int32 StepIndex = 0;
		TArray<FCustomMapPourContainerSnapshot> Containers;
		TArray<FCustomMapPourVoxel> HeldVoxels;
		TArray<FCustomMapPourVoxel> FallingVoxels;
		TArray<FCustomMapPourVoxel> SettledVoxels;
	};

	/**
	 * Lua 自定义地图倾倒夹具的确定性固定步内核。
	 * 它只计算容器姿态和液体格状态；Actor/ISM/截图均是可替换的表现适配器。
	 */
	class MATTERFLUX_API FCustomMapPourSimulation
	{
	public:
		bool Initialize(
			const FCustomMapScene& Scene,
			const FMatterFluxContentRegistry& Registry,
			int32 Seed,
			FString& OutError);
		void Step();
		void GetSnapshot(FCustomMapPourSnapshot& OutSnapshot) const;
		int32 GetStepIndex() const { return StepIndex; }

	private:
		struct FContainerRuntime
		{
			FCustomMapPourContainer Definition;
			int32 InitialCells = 0;
			int32 HeldCells = 0;
			int32 ReleaseCursor = 0;
		};

		struct FFallingVoxel
		{
			FName MaterialId;
			FVector Position = FVector::ZeroVector;
			FVector Velocity = FVector::ZeroVector;
			int32 Serial = 0;
		};

		float GetTiltDegrees(const FContainerRuntime& Container) const;
		FTransform GetContainerTransform(const FContainerRuntime& Container) const;
		void ReleaseFrom(FContainerRuntime& Container, float TiltDegrees);
		void SettleVoxel(const FFallingVoxel& Voxel);

		float CellSize = 24.0f;
		float VoxelSize = 18.0f;
		float GroundHeight = 12.0f;
		FBox2D SettlementBounds = FBox2D(ForceInit);
		int32 Seed = 0;
		int32 StepIndex = 0;
		int32 NextSerial = 0;
		TArray<FContainerRuntime> Containers;
		TArray<FFallingVoxel> Falling;
		TMap<FName, float> MaterialDensities;
		TMap<FIntPoint, TArray<FName>> SettledColumns;
	};
}
