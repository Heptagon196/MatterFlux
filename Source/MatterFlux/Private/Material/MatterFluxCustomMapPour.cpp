#include "Material/MatterFluxCustomMapPour.h"

#include "MatterFluxContentTypes.h"

namespace MatterFlux::Material
{
	bool FCustomMapPourSimulation::Initialize(
		const FCustomMapScene& Scene,
		const FMatterFluxContentRegistry& Registry,
		const int32 InSeed,
		FString& OutError)
	{
		OutError.Reset();
		Containers.Reset();
		Falling.Reset();
		MaterialDensities.Reset();
		SettledColumns.Reset();
		StepIndex = 0;
		NextSerial = 0;
		Seed = InSeed;
		CellSize = Scene.CellSizeCentimeters;
		VoxelSize = CellSize * 0.72f;
		GroundHeight = CellSize * 0.5f;
		SettlementBounds = FBox2D(ForceInit);
		for (const FCustomMapSceneBox& Box : Scene.Boxes)
		{
			if (Box.Id == TEXT("catch.floor"))
			{
				const FVector2D HalfSize(Box.Size.X * 0.5f, Box.Size.Y * 0.5f);
				const FVector2D Margin(VoxelSize * 0.5f);
				SettlementBounds = FBox2D(
					FVector2D(Box.Center.X, Box.Center.Y) - HalfSize + Margin,
					FVector2D(Box.Center.X, Box.Center.Y) + HalfSize - Margin);
				GroundHeight = Box.Center.Z + Box.Size.Z * 0.5f;
				break;
			}
		}
		if (Scene.PourContainers.IsEmpty() || CellSize <= 0.0f)
		{
			OutError = TEXT("Custom-map pour simulation needs at least one valid container");
			return false;
		}

		for (const FCustomMapPourContainer& Definition : Scene.PourContainers)
		{
			const FMatterFluxMaterialDefinition* ContainerMaterial =
				Registry.Materials.Find(Definition.ContainerMaterialId);
			const FMatterFluxMaterialDefinition* LiquidMaterial =
				Registry.Materials.Find(Definition.LiquidMaterialId);
			if (!ContainerMaterial || !LiquidMaterial)
			{
				OutError = FString::Printf(
					TEXT("Pour container '%s' references a missing material"),
					*Definition.Id.ToString());
				return false;
			}
			FContainerRuntime& Runtime = Containers.Emplace_GetRef();
			Runtime.Definition = Definition;
			Runtime.InitialCells = Definition.InteriorSizeCells.X
				* Definition.InteriorSizeCells.Y
				* Definition.InteriorSizeCells.Z;
			Runtime.HeldCells = Runtime.InitialCells;
			MaterialDensities.Add(
				Definition.LiquidMaterialId,
				LiquidMaterial->Density);
		}
		Containers.Sort([](const FContainerRuntime& A, const FContainerRuntime& B)
		{
			return A.Definition.Id.LexicalLess(B.Definition.Id);
		});
		return true;
	}

	float FCustomMapPourSimulation::GetTiltDegrees(
		const FContainerRuntime& Container) const
	{
		if (StepIndex <= Container.Definition.StartStep)
		{
			return 0.0f;
		}
		const float Alpha = FMath::Clamp(
			static_cast<float>(StepIndex - Container.Definition.StartStep)
				/ Container.Definition.TiltDurationSteps,
			0.0f,
			1.0f);
		return Container.Definition.TiltDegrees * Alpha;
	}

	FTransform FCustomMapPourSimulation::GetContainerTransform(
		const FContainerRuntime& Container) const
	{
		const float Radians = FMath::DegreesToRadians(GetTiltDegrees(Container));
		return FTransform(
			FQuat(FVector::YAxisVector, Radians),
			Container.Definition.Center);
	}

	void FCustomMapPourSimulation::ReleaseFrom(
		FContainerRuntime& Container,
		const float TiltDegrees)
	{
		if (TiltDegrees < 18.0f || Container.HeldCells <= 0)
		{
			return;
		}
		const int32 ReleaseCount = FMath::Min(
			Container.Definition.PourCellsPerStep,
			Container.HeldCells);
		const FTransform Transform = GetContainerTransform(Container);
		const FVector Interior = FVector(Container.Definition.InteriorSizeCells)
			* CellSize;
		for (int32 Index = 0; Index < ReleaseCount; ++Index)
		{
			const int32 Cursor = Container.ReleaseCursor++;
			const int32 YCell = Cursor % Container.Definition.InteriorSizeCells.Y;
			const int32 Layer = (Cursor / Container.Definition.InteriorSizeCells.Y)
				% Container.Definition.InteriorSizeCells.Z;
			const float HashJitter = static_cast<float>(
				GetTypeHash(HashCombineFast(
					GetTypeHash(Seed),
					GetTypeHash(Cursor))) & 255) / 255.0f - 0.5f;
			const FVector LocalLip(
				Interior.X * 0.5f + CellSize * 0.15f,
				(YCell + 0.5f - Container.Definition.InteriorSizeCells.Y * 0.5f)
					* CellSize + HashJitter * CellSize * 0.18f,
				Interior.Z * 0.5f - (Layer % 2) * CellSize * 0.22f);
			FFallingVoxel& Voxel = Falling.Emplace_GetRef();
			Voxel.MaterialId = Container.Definition.LiquidMaterialId;
			Voxel.Position = Transform.TransformPosition(LocalLip);
			Voxel.Velocity = Transform.TransformVectorNoScale(
				FVector(105.0f, HashJitter * 28.0f, -25.0f));
			Voxel.Serial = NextSerial++;
		}
		Container.HeldCells -= ReleaseCount;
	}

	void FCustomMapPourSimulation::SettleVoxel(const FFallingVoxel& Voxel)
	{
		const FIntPoint ImpactCell(
			FMath::RoundToInt(Voxel.Position.X / VoxelSize),
			FMath::RoundToInt(Voxel.Position.Y / VoxelSize));
		// 落地后优先占据附近最低列，而不是在命中点无限向上堆成液体塔。
		// 有界圆盘保证成本固定；外圈的 seed 哈希裁剪产生可复现的不规则水边。
		constexpr int32 SpreadRadius = 7;
		FIntPoint BestCell = ImpactCell;
		int32 BestHeight = MAX_int32;
		int32 BestDistanceSquared = MAX_int32;
		uint32 BestTie = MAX_uint32;
		for (int32 OffsetY = -SpreadRadius; OffsetY <= SpreadRadius; ++OffsetY)
		{
			for (int32 OffsetX = -SpreadRadius; OffsetX <= SpreadRadius; ++OffsetX)
			{
				const int32 DistanceSquared =
					OffsetX * OffsetX + OffsetY * OffsetY;
				if (DistanceSquared > SpreadRadius * SpreadRadius)
				{
					continue;
				}
				const FIntPoint Candidate = ImpactCell
					+ FIntPoint(OffsetX, OffsetY);
				const FVector2D CandidatePosition(
					Candidate.X * VoxelSize,
					Candidate.Y * VoxelSize);
				if (SettlementBounds.bIsValid
					&& !SettlementBounds.IsInsideOrOn(CandidatePosition))
				{
					continue;
				}
				const uint32 Tie = GetTypeHash(HashCombineFast(
					GetTypeHash(Seed), GetTypeHash(Candidate)));
				if (DistanceSquared > (SpreadRadius - 1) * (SpreadRadius - 1)
					&& (Tie & 3u) == 0u)
				{
					continue;
				}
				const TArray<FName>* Existing = SettledColumns.Find(Candidate);
				const int32 Height = Existing ? Existing->Num() : 0;
				if (Height < BestHeight
					|| (Height == BestHeight
						&& DistanceSquared < BestDistanceSquared)
					|| (Height == BestHeight
						&& DistanceSquared == BestDistanceSquared
						&& Tie < BestTie))
				{
					BestCell = Candidate;
					BestHeight = Height;
					BestDistanceSquared = DistanceSquared;
					BestTie = Tie;
				}
			}
		}
		TArray<FName>& Column = SettledColumns.FindOrAdd(BestCell);
		Column.Add(Voxel.MaterialId);
		Column.Sort([this](const FName A, const FName B)
		{
			const float DensityA = MaterialDensities.FindRef(A);
			const float DensityB = MaterialDensities.FindRef(B);
			if (!FMath::IsNearlyEqual(DensityA, DensityB))
			{
				return DensityA > DensityB;
			}
			return A.LexicalLess(B);
		});
	}

	void FCustomMapPourSimulation::Step()
	{
		++StepIndex;
		for (FContainerRuntime& Container : Containers)
		{
			ReleaseFrom(Container, GetTiltDegrees(Container));
		}
		constexpr float DeltaSeconds = 1.0f / 30.0f;
		constexpr float Gravity = 980.0f;
		for (int32 Index = Falling.Num() - 1; Index >= 0; --Index)
		{
			FFallingVoxel& Voxel = Falling[Index];
			Voxel.Velocity.Z -= Gravity * DeltaSeconds;
			Voxel.Position += Voxel.Velocity * DeltaSeconds;
			if (Voxel.Position.Z <= GroundHeight + VoxelSize * 0.5f)
			{
				SettleVoxel(Voxel);
				Falling.RemoveAt(Index);
			}
		}
	}

	void FCustomMapPourSimulation::GetSnapshot(
		FCustomMapPourSnapshot& OutSnapshot) const
	{
		OutSnapshot = {};
		OutSnapshot.StepIndex = StepIndex;
		for (const FContainerRuntime& Container : Containers)
		{
			FCustomMapPourContainerSnapshot& ContainerSnapshot =
				OutSnapshot.Containers.Emplace_GetRef();
			ContainerSnapshot.Id = Container.Definition.Id;
			ContainerSnapshot.ContainerMaterialId =
				Container.Definition.ContainerMaterialId;
			ContainerSnapshot.LiquidMaterialId =
				Container.Definition.LiquidMaterialId;
			ContainerSnapshot.Transform = GetContainerTransform(Container);
			ContainerSnapshot.InteriorSize =
				FVector(Container.Definition.InteriorSizeCells) * CellSize;
			ContainerSnapshot.WallThickness = CellSize * 0.32f;
			ContainerSnapshot.TiltDegrees = GetTiltDegrees(Container);
			ContainerSnapshot.InitialLiquidCells = Container.InitialCells;
			ContainerSnapshot.HeldLiquidCells = Container.HeldCells;

			const FIntVector Size = Container.Definition.InteriorSizeCells;
			for (int32 Linear = 0; Linear < Container.HeldCells; ++Linear)
			{
				const int32 X = Linear % Size.X;
				const int32 Y = (Linear / Size.X) % Size.Y;
				const int32 Z = Linear / (Size.X * Size.Y);
				FCustomMapPourVoxel& Voxel =
					OutSnapshot.HeldVoxels.Emplace_GetRef();
				Voxel.MaterialId = Container.Definition.LiquidMaterialId;
				Voxel.Size = FVector(VoxelSize);
				Voxel.Rotation = ContainerSnapshot.Transform.GetRotation();
				const FVector LocalPosition(
					(X + 0.5f - Size.X * 0.5f) * CellSize,
					(Y + 0.5f - Size.Y * 0.5f) * CellSize,
					(Z + 0.5f - Size.Z * 0.5f) * CellSize);
				Voxel.Position = ContainerSnapshot.Transform.TransformPosition(
					LocalPosition);
			}
		}

		TArray<FFallingVoxel> SortedFalling = Falling;
		SortedFalling.Sort([](const FFallingVoxel& A, const FFallingVoxel& B)
		{
			return A.Serial < B.Serial;
		});
		for (const FFallingVoxel& FallingVoxel : SortedFalling)
		{
			FCustomMapPourVoxel& Voxel =
				OutSnapshot.FallingVoxels.Emplace_GetRef();
			Voxel.MaterialId = FallingVoxel.MaterialId;
			Voxel.Position = FallingVoxel.Position;
			Voxel.Size = FVector(VoxelSize * 0.82f);
		}

		TArray<FIntPoint> SortedCells;
		SettledColumns.GetKeys(SortedCells);
		SortedCells.Sort([](const FIntPoint A, const FIntPoint B)
		{
			return A.X != B.X ? A.X < B.X : A.Y < B.Y;
		});
		for (const FIntPoint Cell : SortedCells)
		{
			const TArray<FName>& Column = SettledColumns.FindChecked(Cell);
			for (int32 Layer = 0; Layer < Column.Num(); ++Layer)
			{
				FCustomMapPourVoxel& Voxel =
					OutSnapshot.SettledVoxels.Emplace_GetRef();
				Voxel.MaterialId = Column[Layer];
				Voxel.Position = FVector(
					Cell.X * VoxelSize,
					Cell.Y * VoxelSize,
					GroundHeight + (Layer + 0.5f) * VoxelSize);
				Voxel.Size = FVector(VoxelSize * 0.96f);
			}
		}
	}
}
