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
	}

	void FLiquidSurfaceProjection::Reset()
	{
		Vertices.Reset();
		Triangles.Reset();
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
		OutProjection.Reset();
		if (Cells.IsEmpty() || CellSize <= 0.0f || FullColumnHeight <= 0.0f)
		{
			return;
		}

		// Material occupancy and amount are the only liquid facts. Duplicate
		// coordinates can occur during a representation hand-off, so collapse only
		// those duplicates; do not fill vacancies, select a body-wide level, or
		// rewrite a current particle column for presentation.
		TMap<FIntPoint, float> CurrentHeights;
		CurrentHeights.Reserve(Cells.Num());
		for (const Material::FCellSnapshot& Cell : Cells)
		{
			const float SurfaceHeight = static_cast<float>(Cell.SupportHeight)
				+ FullColumnHeight
					* (static_cast<float>(Cell.Amount) / 255.0f);
			if (float* Existing = CurrentHeights.Find(Cell.WorldCell))
			{
				*Existing = FMath::Max(*Existing, SurfaceHeight);
			}
			else
			{
				CurrentHeights.Add(Cell.WorldCell, SurfaceHeight);
			}
		}

		TArray<float> CurrentSurfaceValues;
		CurrentSurfaceValues.Reserve(CurrentHeights.Num());
		for (const TPair<FIntPoint, float>& Pair : CurrentHeights)
		{
			CurrentSurfaceValues.Add(Pair.Value);
		}
		const float CurrentMedian = Median(CurrentSurfaceValues);
		OutProjection.CanonicalMedianSurfaceHeight = CurrentMedian;
		OutProjection.ProjectedCanonicalMedianSurfaceHeight = CurrentMedian;
		OutProjection.MedianCanonicalHeightOffset = 0.0f;
		OutProjection.MaximumAbsoluteCanonicalHeightOffset = 0.0f;
		OutProjection.ProjectedCellCount = CurrentHeights.Num();
		OutProjection.SurfaceHeights = CurrentHeights;

		TArray<FIntPoint> CurrentCells;
		CurrentHeights.GetKeys(CurrentCells);
		CurrentCells.Sort([](const FIntPoint Left, const FIntPoint Right)
		{
			return Left.Y != Right.Y ? Left.Y < Right.Y : Left.X < Right.X;
		});

		// Shape extraction is local to this snapshot. Neighbouring columns share
		// vertices only while the complete patch stays inside one render-height
		// band. This smooths the boundary between nearby particles without turning
		// a river, waterfall, and lake into one stretched or flattened sheet.
		TSet<FIntPoint> Visited;
		Visited.Reserve(CurrentCells.Num());
		const float MaximumContinuousSurfaceStep = FMath::Max(
			4.0f, FMath::Min(CellSize * 2.0f, FullColumnHeight * 0.25f));
		OutProjection.Vertices.Reserve(CurrentCells.Num() * 2);
		OutProjection.Triangles.Reserve(CurrentCells.Num() * 6);
		OutProjection.UVs.Reserve(CurrentCells.Num() * 2);

		for (const FIntPoint Root : CurrentCells)
		{
			if (Visited.Contains(Root))
			{
				continue;
			}
			++OutProjection.SurfacePatchCount;

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
			TMap<FIntPoint, TArray<float, TInlineAllocator<4>>> CornerContributions;
			CornerContributions.Reserve(ShapeCells.Num() * 2);
			for (const FIntPoint Cell : ShapeCells)
			{
				const float Height = CurrentHeights.FindChecked(Cell);
				const FIntPoint Corners[] = {
					Cell,
					Cell + FIntPoint(1, 0),
					Cell + FIntPoint(1, 1),
					Cell + FIntPoint(0, 1) };
				for (const FIntPoint Corner : Corners)
				{
					CornerContributions.FindOrAdd(Corner).Add(Height);
				}
			}

			TMap<FIntPoint, int32> CornerIndices;
			CornerIndices.Reserve(ShapeCells.Num() * 2);
			for (const FIntPoint Cell : ShapeCells)
			{
				const FIntPoint Corners[] = {
					Cell,
					Cell + FIntPoint(1, 0),
					Cell + FIntPoint(1, 1),
					Cell + FIntPoint(0, 1) };
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
					const TArray<float, TInlineAllocator<4>>& Contributions =
						CornerContributions.FindChecked(Corner);
					float CornerHeight = 0.0f;
					for (const float Height : Contributions)
					{
						CornerHeight += Height;
					}
					CornerHeight /= static_cast<float>(Contributions.Num());
					OutProjection.Vertices.Add(FVector(
						static_cast<float>(Corner.X) * CellSize,
						static_cast<float>(Corner.Y) * CellSize,
						CornerHeight));
					OutProjection.UVs.Add(FVector2D(
						static_cast<float>(Corner.X),
						static_cast<float>(Corner.Y)));
				}
				OutProjection.Triangles.Append({
					CellCornerIndices[0],
					CellCornerIndices[2],
					CellCornerIndices[1],
					CellCornerIndices[0],
					CellCornerIndices[3],
					CellCornerIndices[2] });
			}
		}

		OutProjection.TopTriangleIndexCount = OutProjection.Triangles.Num();
	}
}
