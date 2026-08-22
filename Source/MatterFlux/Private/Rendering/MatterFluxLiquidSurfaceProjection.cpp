#include "Rendering/MatterFluxLiquidSurfaceProjection.h"

namespace MatterFlux::Rendering
{
	namespace
	{
		struct FPatchCornerKey
		{
			FIntPoint Corner = FIntPoint::ZeroValue;
			int32 Patch = 0;

			friend bool operator==(
				const FPatchCornerKey& Left,
				const FPatchCornerKey& Right)
			{
				return Left.Patch == Right.Patch
					&& Left.Corner == Right.Corner;
			}

			friend uint32 GetTypeHash(const FPatchCornerKey& Key)
			{
				return HashCombine(GetTypeHash(Key.Corner), GetTypeHash(Key.Patch));
			}
		};

		float Median(TArray<float>& Values)
		{
			if (Values.IsEmpty())
			{
				return 0.0f;
			}
			Values.Sort();
			const int32 Middle = Values.Num() / 2;
			return (Values.Num() & 1) != 0
				? Values[Middle]
				: (Values[Middle - 1] + Values[Middle]) * 0.5f;
		}

		const FIntPoint ShapeNeighbors[] = {
			FIntPoint(1, 0),
			FIntPoint(-1, 0),
			FIntPoint(0, 1),
			FIntPoint(0, -1) };

		bool IsOwnedCell(const FIntPoint Cell, const FIntRect* OwnedBounds)
		{
			return !OwnedBounds || OwnedBounds->Contains(Cell);
		}

		void AddSideQuad(
			FLiquidSurfaceProjection& Projection,
			const FVector& TopStart,
			const FVector& TopEnd,
			const float BottomStartZ,
			const float BottomEndZ,
			const FVector& Normal,
			const float CellSize)
		{
			if (TopStart.Z <= BottomStartZ + KINDA_SMALL_NUMBER
				&& TopEnd.Z <= BottomEndZ + KINDA_SMALL_NUMBER)
			{
				return;
			}
			const FVector BottomStart(TopStart.X, TopStart.Y,
				FMath::Min(BottomStartZ, TopStart.Z));
			const FVector BottomEnd(TopEnd.X, TopEnd.Y,
				FMath::Min(BottomEndZ, TopEnd.Z));
			const int32 FirstVertex = Projection.Vertices.Num();
			Projection.Vertices.Append({ TopStart, TopEnd, BottomEnd, BottomStart });
			Projection.Normals.Append({ Normal, Normal, Normal, Normal });
			const float EdgeLength = FVector::Distance(TopStart, TopEnd)
				/ FMath::Max(CellSize, 1.0f);
			const float StartDepth = (TopStart.Z - BottomStart.Z)
				/ FMath::Max(CellSize, 1.0f);
			const float EndDepth = (TopEnd.Z - BottomEnd.Z)
				/ FMath::Max(CellSize, 1.0f);
			Projection.UVs.Append({
				FVector2D(0.0f, 0.0f),
				FVector2D(EdgeLength, 0.0f),
				FVector2D(EdgeLength, EndDepth),
				FVector2D(0.0f, StartDepth) });
			Projection.Triangles.Append({
				FirstVertex,
				FirstVertex + 2,
				FirstVertex + 1,
				FirstVertex,
				FirstVertex + 3,
				FirstVertex + 2 });
		}

		void BuildLiquidProjection(
			const TConstArrayView<Material::FCellSnapshot> Cells,
			const float CellSize,
			const float FullColumnHeight,
			const FIntRect* OwnedBounds,
			FLiquidSurfaceProjection& OutProjection)
		{
			OutProjection.Reset();
			if (Cells.IsEmpty() || CellSize <= 0.0f || FullColumnHeight <= 0.0f)
			{
				return;
			}

			TMap<FIntPoint, float> CurrentHeights;
			TMap<FIntPoint, float> CurrentSupports;
			CurrentHeights.Reserve(Cells.Num());
			CurrentSupports.Reserve(Cells.Num());
			for (const Material::FCellSnapshot& Cell : Cells)
			{
				const float SurfaceHeight = static_cast<float>(Cell.SupportHeight)
					+ FullColumnHeight
						* (static_cast<float>(Cell.Amount) / 255.0f);
				if (float* Existing = CurrentHeights.Find(Cell.WorldCell))
				{
					*Existing = FMath::Max(*Existing, SurfaceHeight);
					CurrentSupports.FindChecked(Cell.WorldCell) = FMath::Min(
						CurrentSupports.FindChecked(Cell.WorldCell),
						static_cast<float>(Cell.SupportHeight));
				}
				else
				{
					CurrentHeights.Add(Cell.WorldCell, SurfaceHeight);
					CurrentSupports.Add(Cell.WorldCell,
						static_cast<float>(Cell.SupportHeight));
				}
			}

			TArray<float> CurrentSurfaceValues;
			for (const TPair<FIntPoint, float>& Pair : CurrentHeights)
			{
				if (IsOwnedCell(Pair.Key, OwnedBounds))
				{
					CurrentSurfaceValues.Add(Pair.Value);
					OutProjection.SurfaceHeights.Add(Pair.Key, Pair.Value);
				}
			}
			const float CurrentMedian = Median(CurrentSurfaceValues);
			OutProjection.CanonicalMedianSurfaceHeight = CurrentMedian;
			OutProjection.ProjectedCanonicalMedianSurfaceHeight = CurrentMedian;
			OutProjection.ProjectedCellCount = OutProjection.SurfaceHeights.Num();

			TArray<FIntPoint> CurrentCells;
			CurrentHeights.GetKeys(CurrentCells);
			CurrentCells.Sort([](const FIntPoint Left, const FIntPoint Right)
			{
				return Left.Y != Right.Y ? Left.Y < Right.Y : Left.X < Right.X;
			});

			TSet<FIntPoint> Visited;
			TMap<FIntPoint, int32> CellPatches;
			TMap<FPatchCornerKey, float> PatchCornerHeights;
			const float MaximumContinuousSurfaceStep = FMath::Max(
				4.0f, FMath::Min(CellSize * 2.0f, FullColumnHeight * 0.25f));
			int32 NextPatch = 0;
			for (const FIntPoint Root : CurrentCells)
			{
				if (Visited.Contains(Root))
				{
					continue;
				}
				const int32 Patch = NextPatch++;
				TArray<FIntPoint> ShapeCells{ Root };
				float PatchMinimumHeight = CurrentHeights.FindChecked(Root);
				float PatchMaximumHeight = PatchMinimumHeight;
				Visited.Add(Root);
				for (int32 QueueIndex = 0; QueueIndex < ShapeCells.Num(); ++QueueIndex)
				{
					const FIntPoint Current = ShapeCells[QueueIndex];
					for (const FIntPoint Offset : ShapeNeighbors)
					{
						const FIntPoint Neighbor = Current + Offset;
						const float* NeighborHeight = CurrentHeights.Find(Neighbor);
						if (!NeighborHeight || Visited.Contains(Neighbor))
						{
							continue;
						}
						const float ExpandedMinimum = FMath::Min(
							PatchMinimumHeight, *NeighborHeight);
						const float ExpandedMaximum = FMath::Max(
							PatchMaximumHeight, *NeighborHeight);
						if (ExpandedMaximum - ExpandedMinimum
							<= MaximumContinuousSurfaceStep)
						{
							PatchMinimumHeight = ExpandedMinimum;
							PatchMaximumHeight = ExpandedMaximum;
							Visited.Add(Neighbor);
							ShapeCells.Add(Neighbor);
						}
					}
				}
				ShapeCells.Sort([](const FIntPoint Left, const FIntPoint Right)
				{
					return Left.Y != Right.Y ? Left.Y < Right.Y : Left.X < Right.X;
				});
				bool bPatchIsOwned = false;
				TMap<FIntPoint, TArray<float, TInlineAllocator<4>>> CornerContributions;
				for (const FIntPoint Cell : ShapeCells)
				{
					CellPatches.Add(Cell, Patch);
					bPatchIsOwned |= IsOwnedCell(Cell, OwnedBounds);
					const float Height = CurrentHeights.FindChecked(Cell);
					const FIntPoint Corners[] = {
						Cell, Cell + FIntPoint(1, 0),
						Cell + FIntPoint(1, 1), Cell + FIntPoint(0, 1) };
					for (const FIntPoint Corner : Corners)
					{
						CornerContributions.FindOrAdd(Corner).Add(Height);
					}
				}
				OutProjection.SurfacePatchCount += bPatchIsOwned ? 1 : 0;
				for (const TPair<FIntPoint, TArray<float, TInlineAllocator<4>>>& Pair
					: CornerContributions)
				{
					float Height = 0.0f;
					for (const float Contribution : Pair.Value)
					{
						Height += Contribution;
					}
					PatchCornerHeights.Add({ Pair.Key, Patch },
						Height / static_cast<float>(Pair.Value.Num()));
				}

				TMap<FIntPoint, int32> CornerIndices;
				for (const FIntPoint Cell : ShapeCells)
				{
					if (!IsOwnedCell(Cell, OwnedBounds))
					{
						continue;
					}
					const FIntPoint Corners[] = {
						Cell, Cell + FIntPoint(1, 0),
						Cell + FIntPoint(1, 1), Cell + FIntPoint(0, 1) };
					int32 CellCornerIndices[4];
					for (int32 CornerIndex = 0; CornerIndex < 4; ++CornerIndex)
					{
						const FIntPoint Corner = Corners[CornerIndex];
						if (const int32* ExistingIndex = CornerIndices.Find(Corner))
						{
							CellCornerIndices[CornerIndex] = *ExistingIndex;
							continue;
						}
						const int32 VertexIndex = OutProjection.Vertices.Num();
						CornerIndices.Add(Corner, VertexIndex);
						CellCornerIndices[CornerIndex] = VertexIndex;
						OutProjection.Vertices.Add(FVector(
							static_cast<float>(Corner.X) * CellSize,
							static_cast<float>(Corner.Y) * CellSize,
							PatchCornerHeights.FindChecked({ Corner, Patch })));
						OutProjection.Normals.Add(FVector::UpVector);
						OutProjection.UVs.Add(FVector2D(
							static_cast<float>(Corner.X),
							static_cast<float>(Corner.Y)));
					}
					OutProjection.Triangles.Append({
						CellCornerIndices[0], CellCornerIndices[2], CellCornerIndices[1],
						CellCornerIndices[0], CellCornerIndices[3], CellCornerIndices[2] });
				}
			}

			OutProjection.TopTriangleIndexCount = OutProjection.Triangles.Num();
			const FIntPoint EdgeOffsets[] = {
				FIntPoint(0, -1), FIntPoint(1, 0),
				FIntPoint(0, 1), FIntPoint(-1, 0) };
			const FVector EdgeNormals[] = {
				-FVector::YAxisVector, FVector::XAxisVector,
				FVector::YAxisVector, -FVector::XAxisVector };
			for (const FIntPoint Cell : CurrentCells)
			{
				if (!IsOwnedCell(Cell, OwnedBounds))
				{
					continue;
				}
				const int32 Patch = CellPatches.FindChecked(Cell);
				const float Surface = CurrentHeights.FindChecked(Cell);
				const float Support = CurrentSupports.FindChecked(Cell);
				const FIntPoint EdgeCorners[4][2] = {
					{ Cell + FIntPoint(1, 0), Cell },
					{ Cell + FIntPoint(1, 1), Cell + FIntPoint(1, 0) },
					{ Cell + FIntPoint(0, 1), Cell + FIntPoint(1, 1) },
					{ Cell, Cell + FIntPoint(0, 1) } };
				for (int32 Edge = 0; Edge < 4; ++Edge)
				{
					const FIntPoint Neighbor = Cell + EdgeOffsets[Edge];
					const float* NeighborSurface = CurrentHeights.Find(Neighbor);
					const int32* NeighborPatch = CellPatches.Find(Neighbor);
					if (NeighborSurface && NeighborPatch && *NeighborPatch == Patch)
					{
						continue;
					}
					if (NeighborSurface && *NeighborSurface >= Surface - KINDA_SMALL_NUMBER)
					{
						continue;
					}
					const float Bottom = NeighborSurface
						? FMath::Max(Support, *NeighborSurface)
						: Support;
					const FIntPoint StartCorner = EdgeCorners[Edge][0];
					const FIntPoint EndCorner = EdgeCorners[Edge][1];
					AddSideQuad(
						OutProjection,
						FVector(StartCorner.X * CellSize, StartCorner.Y * CellSize,
							PatchCornerHeights.FindChecked({ StartCorner, Patch })),
						FVector(EndCorner.X * CellSize, EndCorner.Y * CellSize,
							PatchCornerHeights.FindChecked({ EndCorner, Patch })),
						Bottom,
						Bottom,
						EdgeNormals[Edge],
						CellSize);
				}
			}
		}
	}

	void FLiquidSurfaceProjection::Reset()
	{
		Vertices.Reset();
		Triangles.Reset();
		Normals.Reset();
		UVs.Reset();
		SurfaceHeights.Reset();
		TopTriangleIndexCount = 0;
		SurfacePatchCount = 0;
		ProjectedCellCount = 0;
		CanonicalMedianSurfaceHeight = 0.0f;
		ProjectedCanonicalMedianSurfaceHeight = 0.0f;
		MedianCanonicalHeightOffset = 0.0f;
		MaximumAbsoluteCanonicalHeightOffset = 0.0f;
	}

	void BuildLiquidSurfaceProjection(
		const TConstArrayView<Material::FCellSnapshot> Cells,
		const float CellSize,
		const float FullColumnHeight,
		FLiquidSurfaceProjection& OutProjection)
	{
		BuildLiquidProjection(Cells, CellSize, FullColumnHeight, nullptr,
			OutProjection);
	}

	void BuildLiquidSurfaceChunkProjection(
		const TConstArrayView<Material::FCellSnapshot> CellsWithHalo,
		const float CellSize,
		const float FullColumnHeight,
		const FIntPoint ChunkCoordinate,
		const int32 ChunkSize,
		FLiquidSurfaceProjection& OutProjection)
	{
		if (ChunkSize <= 0)
		{
			OutProjection.Reset();
			return;
		}
		const FIntPoint Minimum = ChunkCoordinate * ChunkSize;
		const FIntRect OwnedBounds(Minimum, Minimum + FIntPoint(ChunkSize));
		BuildLiquidProjection(CellsWithHalo, CellSize, FullColumnHeight,
			&OwnedBounds, OutProjection);
	}

	void PartitionLiquidProjectionChunksCheckerboard(
		const TConstArrayView<FIntPoint> Chunks,
		TArray<FIntPoint>& OutEvenChunks,
		TArray<FIntPoint>& OutOddChunks)
	{
		OutEvenChunks.Reset();
		OutOddChunks.Reset();
		TSet<FIntPoint> UniqueChunks;
		for (const FIntPoint Chunk : Chunks)
		{
			UniqueChunks.Add(Chunk);
		}
		TArray<FIntPoint> SortedChunks = UniqueChunks.Array();
		SortedChunks.Sort([](const FIntPoint A, const FIntPoint B)
		{
			return A.Y != B.Y ? A.Y < B.Y : A.X < B.X;
		});
		for (const FIntPoint Chunk : SortedChunks)
		{
			const bool bOdd =
				((static_cast<uint32>(Chunk.X) + static_cast<uint32>(Chunk.Y)) & 1u)
				!= 0u;
			(bOdd ? OutOddChunks : OutEvenChunks).Add(Chunk);
		}
	}
}
