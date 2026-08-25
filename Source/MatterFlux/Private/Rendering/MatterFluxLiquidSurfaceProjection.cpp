#include "Rendering/MatterFluxLiquidSurfaceProjection.h"

namespace MatterFlux::Rendering
{
	namespace
	{
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
			const float CellSize,
			const float ColumnDepth)
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
			Projection.ColumnDepths.Append({
				ColumnDepth, ColumnDepth, ColumnDepth, ColumnDepth });
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
			TMap<FIntPoint, float> CurrentDepths;
			CurrentHeights.Reserve(Cells.Num());
			CurrentSupports.Reserve(Cells.Num());
			CurrentDepths.Reserve(Cells.Num());
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
			for (const TPair<FIntPoint, float>& Pair : CurrentHeights)
			{
				CurrentDepths.Add(
					Pair.Key,
					FMath::Max(
						Pair.Value - CurrentSupports.FindChecked(Pair.Key),
						0.0f));
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
			const float MaximumContinuousSurfaceStep = FMath::Max(
				4.0f, FMath::Min(CellSize * 2.0f, FullColumnHeight * 0.25f));
			for (const FIntPoint Root : CurrentCells)
			{
				if (Visited.Contains(Root))
				{
					continue;
				}
				TArray<FIntPoint> ShapeCells{ Root };
				Visited.Add(Root);
				for (int32 QueueIndex = 0; QueueIndex < ShapeCells.Num(); ++QueueIndex)
				{
					const FIntPoint Current = ShapeCells[QueueIndex];
					const float CurrentHeight =
						CurrentHeights.FindChecked(Current);
					for (const FIntPoint Offset : ShapeNeighbors)
					{
						const FIntPoint Neighbor = Current + Offset;
						const float* NeighborHeight = CurrentHeights.Find(Neighbor);
						if (!NeighborHeight || Visited.Contains(Neighbor))
						{
							continue;
						}
						// Continuity is a local edge fact. A long river may lose far
						// more height than one column while every neighboring pair is
						// still a gentle slope. Bounding the total component range cut
						// those rivers into arbitrary bands and emitted vertical walls
						// at each band boundary.
						if (FMath::Abs(*NeighborHeight - CurrentHeight)
							<= MaximumContinuousSurfaceStep)
						{
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
				for (const FIntPoint Cell : ShapeCells)
				{
					bPatchIsOwned |= IsOwnedCell(Cell, OwnedBounds);
				}
				OutProjection.SurfacePatchCount += bPatchIsOwned ? 1 : 0;

				for (const FIntPoint Cell : ShapeCells)
				{
					if (!IsOwnedCell(Cell, OwnedBounds))
					{
						continue;
					}
					const FIntPoint Corners[] = {
						Cell, Cell + FIntPoint(1, 0),
						Cell + FIntPoint(1, 1), Cell + FIntPoint(0, 1) };
					const float Height = CurrentHeights.FindChecked(Cell);
					const float ColumnDepth = CurrentDepths.FindChecked(Cell);
					int32 CellCornerIndices[4];
					for (int32 CornerIndex = 0; CornerIndex < 4; ++CornerIndex)
					{
						const FIntPoint Corner = Corners[CornerIndex];
						const int32 VertexIndex = OutProjection.Vertices.Num();
						CellCornerIndices[CornerIndex] = VertexIndex;
						OutProjection.Vertices.Add(FVector(
							static_cast<float>(Corner.X) * CellSize,
							static_cast<float>(Corner.Y) * CellSize,
							Height));
						OutProjection.Normals.Add(FVector::UpVector);
						OutProjection.ColumnDepths.Add(ColumnDepth);
						// World-continuous UVs let equal-height particles read as
						// one flat body. Their vertices remain independent, so an
						// unequal neighbor produces a hard voxel step, never a
						// curved interpolation between particle heights.
						OutProjection.UVs.Add(FVector2D(
							static_cast<float>(Corner.X),
							static_cast<float>(Corner.Y)));
					}
					OutProjection.Triangles.Append({
						CellCornerIndices[0], CellCornerIndices[2], CellCornerIndices[1],
						CellCornerIndices[0], CellCornerIndices[3], CellCornerIndices[2] });
				}
			}

			OutProjection.TopVertexCount = OutProjection.Vertices.Num();
			OutProjection.TopTriangleIndexCount = OutProjection.Triangles.Num();
			const FIntPoint EdgeOffsets[] = {
				FIntPoint(0, -1), FIntPoint(1, 0),
				FIntPoint(0, 1), FIntPoint(-1, 0) };
			for (const FIntPoint Cell : CurrentCells)
			{
				if (!IsOwnedCell(Cell, OwnedBounds))
				{
					continue;
				}
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
					if (NeighborSurface
						&& FMath::IsNearlyEqual(*NeighborSurface, Surface))
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
							Surface),
						FVector(EndCorner.X * CellSize, EndCorner.Y * CellSize,
							Surface),
						Bottom,
						Bottom,
						// The voxel-liquid material uses the supplied normal for
						// strong face contrast. Horizontal normals turn short
						// refill skirts nearly black and read as detached debris;
						// the disposable liquid shell keeps one uniform top-lit
						// response while its geometry still carries the step.
						FVector::UpVector,
						CellSize,
						CurrentDepths.FindChecked(Cell));
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
		ColumnDepths.Reset();
		SurfaceHeights.Reset();
		TopVertexCount = 0;
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

	void SelectLiquidProjectionChunksForRebuild(
		const TConstArrayView<FIntPoint> PendingChunks,
		const FIntPoint FocusChunk,
		TArray<FIntPoint>& OutSelectedChunks,
		const TMap<FIntPoint, uint64>* EnqueueOrders,
		const int32 MaximumChunksPerPass)
	{
		TSet<FIntPoint> UniqueChunks;
		UniqueChunks.Append(PendingChunks);
		TArray<FIntPoint> OldestChunks = UniqueChunks.Array();
		OldestChunks.Sort([EnqueueOrders](
			const FIntPoint A,
			const FIntPoint B)
		{
			const uint64 OrderA = EnqueueOrders
				? EnqueueOrders->FindRef(A)
				: 0;
			const uint64 OrderB = EnqueueOrders
				? EnqueueOrders->FindRef(B)
				: 0;
			if (OrderA != OrderB)
			{
				return OrderA < OrderB;
			}
			return A.Y != B.Y ? A.Y < B.Y : A.X < B.X;
		});
		const int32 MaximumBudget = FMath::Clamp(
			MaximumChunksPerPass, 1, 8);
		const int32 Budget = FMath::Min(
			OldestChunks.Num(),
			FMath::Clamp(
				FMath::DivideAndRoundUp(OldestChunks.Num(), 2),
				FMath::Min(2, MaximumBudget),
				MaximumBudget));
		OutSelectedChunks.Reset(Budget);
		// Reserve a bounded share for old trailing wakes so they cannot starve,
		// while keeping most commits near the current gameplay focus. A half/half
		// split visibly delayed the chunks underneath two simultaneously moving
		// bodies when the frame budget was reduced below eight.
		const int32 OldestQuota = FMath::Max(
			FMath::DivideAndRoundUp(Budget, 3), 1);
		for (int32 Index = 0;
			Index < OldestChunks.Num() && OutSelectedChunks.Num() < OldestQuota;
			++Index)
		{
			OutSelectedChunks.Add(OldestChunks[Index]);
		}

		TArray<FIntPoint> NearestChunks = OldestChunks;
		NearestChunks.Sort([FocusChunk, EnqueueOrders](
			const FIntPoint A,
			const FIntPoint B)
		{
			const int32 DistanceA = FMath::Abs(A.X - FocusChunk.X)
				+ FMath::Abs(A.Y - FocusChunk.Y);
			const int32 DistanceB = FMath::Abs(B.X - FocusChunk.X)
				+ FMath::Abs(B.Y - FocusChunk.Y);
			if (DistanceA != DistanceB)
			{
				return DistanceA < DistanceB;
			}
			const uint64 OrderA = EnqueueOrders
				? EnqueueOrders->FindRef(A)
				: 0;
			const uint64 OrderB = EnqueueOrders
				? EnqueueOrders->FindRef(B)
				: 0;
			if (OrderA != OrderB)
			{
				return OrderA < OrderB;
			}
			return A.Y != B.Y ? A.Y < B.Y : A.X < B.X;
		});
		for (const FIntPoint Chunk : NearestChunks)
		{
			if (OutSelectedChunks.Num() >= Budget)
			{
				break;
			}
			OutSelectedChunks.AddUnique(Chunk);
		}
	}
}
