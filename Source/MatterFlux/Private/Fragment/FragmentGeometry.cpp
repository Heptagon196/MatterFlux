#include "Fragment/FragmentGeometry.h"

#include "ConstrainedDelaunay2.h"
#include "Algo/Reverse.h"
#include "Curve/GeneralPolygon2.h"
#include "HAL/UnrealMemory.h"
#include "Polygon2.h"

namespace MatterFlux::FragmentGeometry
{
	namespace
	{
		struct FBoundaryEdge
		{
			FIntPoint Start;
			FIntPoint End;
		};

		int32 ToIndex(const int32 Width, const int32 X, const int32 Y)
		{
			return Y * Width + X;
		}

		bool HasValidMaskDimensions(const TArray<uint8>& Mask, const int32 Width, const int32 Height)
		{
			return Width > 0 && Height > 0
				&& static_cast<int64>(Width) * static_cast<int64>(Height) == Mask.Num();
		}

		FVector CellCenterToLocal(const int32 Width, const int32 Height, const float CellSize, const int32 X, const int32 Y)
		{
			return FVector(
				((static_cast<float>(X) + 0.5f) - static_cast<float>(Width) * 0.5f) * CellSize,
				0.0f,
				((static_cast<float>(Y) + 0.5f) - static_cast<float>(Height) * 0.5f) * CellSize);
		}

		FVector2D CellEdgeToLocal2D(const int32 Width, const int32 Height, const float CellSize, const FIntPoint& Point)
		{
			return FVector2D(
				(static_cast<float>(Point.X) - static_cast<float>(Width) * 0.5f) * CellSize,
				(static_cast<float>(Point.Y) - static_cast<float>(Height) * 0.5f) * CellSize);
		}

		bool IsInsideDamage(const FVector& LocalPoint, const FFragmentDamageShape& Shape)
		{
			switch (Shape.Type)
			{
			case EFragmentDamageShapeType::Circle:
			{
				const FVector ShapeLocal = Shape.WorldTransform.InverseTransformPosition(LocalPoint);
				return ShapeLocal.X * ShapeLocal.X + ShapeLocal.Z * ShapeLocal.Z <= FMath::Square(Shape.Radius);
			}
			case EFragmentDamageShapeType::Box:
			{
				const FVector ShapeLocal = Shape.WorldTransform.InverseTransformPosition(LocalPoint);
				return FMath::Abs(ShapeLocal.X) <= Shape.Extents.X && FMath::Abs(ShapeLocal.Z) <= Shape.Extents.Y;
			}
			case EFragmentDamageShapeType::Line:
			{
				const FVector ShapeLocal = Shape.WorldTransform.InverseTransformPosition(LocalPoint);
				return FMath::Abs(ShapeLocal.X) <= Shape.Extents.X * 0.5f && FMath::Abs(ShapeLocal.Z) <= Shape.Thickness * 0.5f;
			}
			default:
				return false;
			}
		}

		bool LexicographicLess(const FIntPoint& A, const FIntPoint& B)
		{
			return A.X != B.X ? A.X < B.X : A.Y < B.Y;
		}

		bool LexicographicLess(const FVector2D& A, const FVector2D& B)
		{
			return A.X != B.X ? A.X < B.X : A.Y < B.Y;
		}

		double SignedArea(const TArray<FIntPoint>& Loop)
		{
			double TwiceArea = 0.0;
			for (int32 Index = 0; Index < Loop.Num(); ++Index)
			{
				const FIntPoint& A = Loop[Index];
				const FIntPoint& B = Loop[(Index + 1) % Loop.Num()];
				TwiceArea += static_cast<double>(A.X) * B.Y - static_cast<double>(B.X) * A.Y;
			}
			return TwiceArea * 0.5;
		}

		void SimplifyAndCanonicalize(TArray<FIntPoint>& Loop, const bool bWantCounterClockwise)
		{
			if (Loop.Num() > 1 && Loop[0] == Loop.Last())
			{
				Loop.Pop();
			}

			for (int32 Index = Loop.Num() - 1; Index >= 0 && Loop.Num() > 1; --Index)
			{
				if (Loop[Index] == Loop[(Index + 1) % Loop.Num()])
				{
					Loop.RemoveAt(Index);
				}
			}

			bool bRemoved = true;
			while (bRemoved && Loop.Num() >= 3)
			{
				bRemoved = false;
				for (int32 Index = 0; Index < Loop.Num(); ++Index)
				{
					const FIntPoint& Previous = Loop[(Index + Loop.Num() - 1) % Loop.Num()];
					const FIntPoint& Current = Loop[Index];
					const FIntPoint& Next = Loop[(Index + 1) % Loop.Num()];
					const int64 Cross = static_cast<int64>(Current.X - Previous.X) * (Next.Y - Current.Y)
						- static_cast<int64>(Current.Y - Previous.Y) * (Next.X - Current.X);
					if (Cross == 0)
					{
						Loop.RemoveAt(Index);
						bRemoved = true;
						break;
					}
				}
			}

			if (Loop.Num() < 3)
			{
				return;
			}

			if ((SignedArea(Loop) > 0.0) != bWantCounterClockwise)
			{
				Algo::Reverse(Loop);
			}

			int32 MinimumIndex = 0;
			for (int32 Index = 1; Index < Loop.Num(); ++Index)
			{
				if (LexicographicLess(Loop[Index], Loop[MinimumIndex]))
				{
					MinimumIndex = Index;
				}
			}
			if (MinimumIndex > 0)
			{
				TArray<FIntPoint> Rotated;
				Rotated.Reserve(Loop.Num());
				for (int32 Offset = 0; Offset < Loop.Num(); ++Offset)
				{
					Rotated.Add(Loop[(MinimumIndex + Offset) % Loop.Num()]);
				}
				Loop = MoveTemp(Rotated);
			}
		}

		int32 DirectionIndex(const FIntPoint& Delta)
		{
			if (Delta.X > 0) return 0;
			if (Delta.Y > 0) return 1;
			if (Delta.X < 0) return 2;
			return 3;
		}

		int32 TurnRank(const FIntPoint& Incoming, const FIntPoint& Outgoing)
		{
			const int32 Delta = (DirectionIndex(Outgoing) - DirectionIndex(Incoming) + 4) % 4;
			switch (Delta)
			{
			case 1: return 0; // left turn keeps a diagonally touching cell on this loop
			case 0: return 1;
			case 3: return 2;
			default: return 3;
			}
		}

		bool ExtractBoundaryLoops(const TArray<uint8>& Mask, const int32 Width, const int32 Height, TArray<TArray<FIntPoint>>& OutLoops)
		{
			OutLoops.Reset();
			TArray<FBoundaryEdge> Edges;
			for (int32 Y = 0; Y < Height; ++Y)
			{
				for (int32 X = 0; X < Width; ++X)
				{
					if (!IsSolid(Mask, Width, Height, X, Y)) continue;
					if (!IsSolid(Mask, Width, Height, X, Y - 1)) Edges.Add({ FIntPoint(X, Y), FIntPoint(X + 1, Y) });
					if (!IsSolid(Mask, Width, Height, X + 1, Y)) Edges.Add({ FIntPoint(X + 1, Y), FIntPoint(X + 1, Y + 1) });
					if (!IsSolid(Mask, Width, Height, X, Y + 1)) Edges.Add({ FIntPoint(X + 1, Y + 1), FIntPoint(X, Y + 1) });
					if (!IsSolid(Mask, Width, Height, X - 1, Y)) Edges.Add({ FIntPoint(X, Y + 1), FIntPoint(X, Y) });
				}
			}

			Edges.Sort([](const FBoundaryEdge& A, const FBoundaryEdge& B)
			{
				return A.Start != B.Start ? LexicographicLess(A.Start, B.Start) : LexicographicLess(A.End, B.End);
			});
			TMap<FIntPoint, TArray<int32>> OutgoingEdges;
			for (int32 Index = 0; Index < Edges.Num(); ++Index)
			{
				OutgoingEdges.FindOrAdd(Edges[Index].Start).Add(Index);
			}

			TArray<uint8> Used;
			Used.Init(0, Edges.Num());
			for (int32 StartIndex = 0; StartIndex < Edges.Num(); ++StartIndex)
			{
				if (Used[StartIndex] != 0) continue;
				TArray<FIntPoint> Loop;
				int32 CurrentIndex = StartIndex;
				const FIntPoint StartVertex = Edges[StartIndex].Start;
				bool bClosed = false;
				for (int32 Guard = 0; Guard <= Edges.Num(); ++Guard)
				{
					if (Used[CurrentIndex] != 0) return false;
					Used[CurrentIndex] = 1;
					const FBoundaryEdge& Edge = Edges[CurrentIndex];
					Loop.Add(Edge.Start);
					if (Edge.End == StartVertex)
					{
						bClosed = true;
						break;
					}

					const TArray<int32>* Candidates = OutgoingEdges.Find(Edge.End);
					if (!Candidates) return false;
					int32 Best = INDEX_NONE;
					int32 BestRank = MAX_int32;
					const FIntPoint Incoming = Edge.End - Edge.Start;
					for (const int32 Candidate : *Candidates)
					{
						if (Used[Candidate] != 0) continue;
						const int32 Rank = TurnRank(Incoming, Edges[Candidate].End - Edges[Candidate].Start);
						if (Rank < BestRank || (Rank == BestRank && (Best == INDEX_NONE || LexicographicLess(Edges[Candidate].End, Edges[Best].End))))
						{
							Best = Candidate;
							BestRank = Rank;
						}
					}
					if (Best == INDEX_NONE) return false;
					CurrentIndex = Best;
				}
				if (!bClosed || Loop.Num() < 3) return false;
				OutLoops.Add(MoveTemp(Loop));
			}
			return Edges.Num() > 0;
		}

		bool PointInLoop(const FVector2D& Point, const FFragmentContour& Loop)
		{
			bool bInside = false;
			for (int32 A = 0, B = Loop.Vertices.Num() - 1; A < Loop.Vertices.Num(); B = A++)
			{
				const FVector2D& VA = Loop.Vertices[A];
				const FVector2D& VB = Loop.Vertices[B];
				if (((VA.Y > Point.Y) != (VB.Y > Point.Y))
					&& Point.X < (VB.X - VA.X) * (Point.Y - VA.Y) / (VB.Y - VA.Y) + VA.X)
				{
					bInside = !bInside;
				}
			}
			return bInside;
		}

		double ContourArea(const FFragmentContour& Contour)
		{
			double TwiceArea = 0.0;
			for (int32 Index = 0; Index < Contour.Vertices.Num(); ++Index)
			{
				const FVector2D& A = Contour.Vertices[Index];
				const FVector2D& B = Contour.Vertices[(Index + 1) % Contour.Vertices.Num()];
				TwiceArea += A.X * B.Y - B.X * A.Y;
			}
			return TwiceArea * 0.5;
		}

		void BuildConvexHull(const TArray<FVector2D>& Points, FFragmentContour& OutHull)
		{
			TArray<FVector2D> Sorted = Points;
			Sorted.Sort([](const FVector2D& A, const FVector2D& B) { return LexicographicLess(A, B); });
			for (int32 Index = Sorted.Num() - 1; Index > 0; --Index)
			{
				if (Sorted[Index] == Sorted[Index - 1]) Sorted.RemoveAt(Index);
			}
			if (Sorted.Num() < 3)
			{
				OutHull.Vertices = MoveTemp(Sorted);
				return;
			}
			auto Cross = [](const FVector2D& O, const FVector2D& A, const FVector2D& B)
			{
				return FVector2D::CrossProduct(A - O, B - O);
			};
			TArray<FVector2D> Hull;
			for (const FVector2D& Point : Sorted)
			{
				while (Hull.Num() >= 2 && Cross(Hull[Hull.Num() - 2], Hull.Last(), Point) <= 0.0) Hull.Pop();
				Hull.Add(Point);
			}
			const int32 LowerCount = Hull.Num();
			for (int32 Index = Sorted.Num() - 2; Index >= 0; --Index)
			{
				while (Hull.Num() > LowerCount && Cross(Hull[Hull.Num() - 2], Hull.Last(), Sorted[Index]) <= 0.0) Hull.Pop();
				Hull.Add(Sorted[Index]);
			}
			Hull.Pop();
			OutHull.Vertices = MoveTemp(Hull);
		}

		void CanonicalizeTrianglesAndVertices(FFragmentGeometry2D& Geometry)
		{
			TArray<int32> Order;
			for (int32 Index = 0; Index < Geometry.Vertices2D.Num(); ++Index) Order.Add(Index);
			Order.Sort([&Geometry](const int32 A, const int32 B)
			{
				return Geometry.Vertices2D[A] != Geometry.Vertices2D[B]
					? LexicographicLess(Geometry.Vertices2D[A], Geometry.Vertices2D[B]) : A < B;
			});
			TArray<FVector2D> SortedVertices;
			TArray<int32> Remap;
			SortedVertices.SetNum(Order.Num());
			Remap.SetNum(Order.Num());
			for (int32 NewIndex = 0; NewIndex < Order.Num(); ++NewIndex)
			{
				SortedVertices[NewIndex] = Geometry.Vertices2D[Order[NewIndex]];
				Remap[Order[NewIndex]] = NewIndex;
			}
			Geometry.Vertices2D = MoveTemp(SortedVertices);

			TArray<FIntVector> Triangles;
			for (int32 Index = 0; Index < Geometry.TriangleIndices.Num(); Index += 3)
			{
				int32 A = Remap[Geometry.TriangleIndices[Index]];
				int32 B = Remap[Geometry.TriangleIndices[Index + 1]];
				int32 C = Remap[Geometry.TriangleIndices[Index + 2]];
				if (FVector2D::CrossProduct(Geometry.Vertices2D[B] - Geometry.Vertices2D[A], Geometry.Vertices2D[C] - Geometry.Vertices2D[A]) < 0.0) Swap(B, C);
				if (B < A && B < C) { const int32 OldA = A; A = B; B = C; C = OldA; }
				else if (C < A && C < B) { const int32 OldA = A; A = C; C = B; B = OldA; }
				Triangles.Add(FIntVector(A, B, C));
			}
			Triangles.Sort([](const FIntVector& A, const FIntVector& B)
			{
				if (A.X != B.X) return A.X < B.X;
				if (A.Y != B.Y) return A.Y < B.Y;
				return A.Z < B.Z;
			});
			Geometry.TriangleIndices.Reset(Triangles.Num() * 3);
			for (const FIntVector& Triangle : Triangles)
			{
				Geometry.TriangleIndices.Append({ Triangle.X, Triangle.Y, Triangle.Z });
			}
		}
	}

	bool IsSolid(const TArray<uint8>& Mask, const int32 Width, const int32 Height, const int32 X, const int32 Y)
	{
		return X >= 0 && Y >= 0 && X < Width && Y < Height && Mask.IsValidIndex(ToIndex(Width, X, Y)) && Mask[ToIndex(Width, X, Y)] != 0;
	}

	bool ApplyDamageShape(TArray<uint8>& Mask, const int32 Width, const int32 Height, const float CellSize, const FFragmentDamageShape& LocalDamageShape)
	{
		const bool bShapeValid = LocalDamageShape.WorldTransform.IsValid()
			&& ((LocalDamageShape.Type == EFragmentDamageShapeType::Circle
					&& FMath::IsFinite(LocalDamageShape.Radius)
					&& LocalDamageShape.Radius > 0.0f)
				|| (LocalDamageShape.Type == EFragmentDamageShapeType::Box
					&& FMath::IsFinite(LocalDamageShape.Extents.X)
					&& FMath::IsFinite(LocalDamageShape.Extents.Y)
					&& LocalDamageShape.Extents.X > 0.0
					&& LocalDamageShape.Extents.Y > 0.0)
				|| (LocalDamageShape.Type == EFragmentDamageShapeType::Line
					&& FMath::IsFinite(LocalDamageShape.Extents.X)
					&& LocalDamageShape.Extents.X > 0.0
					&& FMath::IsFinite(LocalDamageShape.Thickness)
					&& LocalDamageShape.Thickness > 0.0f));
		if (!HasValidMaskDimensions(Mask, Width, Height)
			|| !FMath::IsFinite(CellSize)
			|| CellSize <= 0.0f
			|| !bShapeValid)
		{
			return false;
		}
		for (const uint8 Cell : Mask)
		{
			if (Cell > 1)
			{
				return false;
			}
		}
		if (LocalDamageShape.Type == EFragmentDamageShapeType::Line
			&& LocalDamageShape.bSingleCellLine)
		{
			// The spell describes an ideal zero-width segment. Rasterize that
			// segment in the target Source's own grid instead of interpreting a
			// world-space thickness as two or three rows on differently-authored
			// materials. DDA produces one 8-connected cell path for horizontal,
			// vertical and rotated Sources alike.
			const float HalfLength = LocalDamageShape.Extents.X * 0.5f;
			const FVector SegmentStart =
				LocalDamageShape.WorldTransform.TransformPosition(
					FVector(-HalfLength, 0.0f, 0.0f));
			const FVector SegmentEnd =
				LocalDamageShape.WorldTransform.TransformPosition(
					FVector(HalfLength, 0.0f, 0.0f));
			const auto ToGrid = [Width, Height, CellSize](const FVector& Point)
			{
				return FVector2D(
					Point.X / CellSize + static_cast<float>(Width) * 0.5f - 0.5f,
					Point.Z / CellSize + static_cast<float>(Height) * 0.5f - 0.5f);
			};
			const FVector2D StartGrid = ToGrid(SegmentStart);
			const FVector2D EndGrid = ToGrid(SegmentEnd);
			const FVector2D GridDelta = EndGrid - StartGrid;
			const int32 StepCount = FMath::Max(
				1,
				FMath::CeilToInt(FMath::Max(
					FMath::Abs(GridDelta.X),
					FMath::Abs(GridDelta.Y))));
			bool bChanged = false;
			for (int32 Step = 0; Step <= StepCount; ++Step)
			{
				const FVector2D GridPoint = FMath::Lerp(
					StartGrid,
					EndGrid,
					static_cast<float>(Step) / static_cast<float>(StepCount));
				const int32 X = FMath::RoundToInt(GridPoint.X);
				const int32 Y = FMath::RoundToInt(GridPoint.Y);
				if (X < 0 || X >= Width || Y < 0 || Y >= Height)
				{
					continue;
				}
				const int32 Index = ToIndex(Width, X, Y);
				if (Mask[Index] != 0)
				{
					Mask[Index] = 0;
					bChanged = true;
				}
			}
			return bChanged;
		}
		bool bChanged = false;
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				const int32 Index = ToIndex(Width, X, Y);
				if (Mask[Index] != 0 && IsInsideDamage(CellCenterToLocal(Width, Height, CellSize, X, Y), LocalDamageShape))
				{
					Mask[Index] = 0;
					bChanged = true;
				}
			}
		}
		return bChanged;
	}

	void ExtractConnectedComponents(const TArray<uint8>& Mask, const int32 Width, const int32 Height, TArray<FFragmentComponent>& OutComponents)
	{
		OutComponents.Reset();
		if (!HasValidMaskDimensions(Mask, Width, Height)) return;
		TArray<uint8> Visited;
		Visited.Init(0, Width * Height);
		const FIntPoint NeighborOffsets[] = {
			FIntPoint(-1, -1), FIntPoint(0, -1), FIntPoint(1, -1), FIntPoint(-1, 0),
			FIntPoint(1, 0), FIntPoint(-1, 1), FIntPoint(0, 1), FIntPoint(1, 1)
		};
		TArray<FIntPoint> Queue;
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				const int32 StartIndex = ToIndex(Width, X, Y);
				if (!IsSolid(Mask, Width, Height, X, Y) || Visited[StartIndex] != 0) continue;
				FFragmentComponent Component;
				Queue.Reset();
				Queue.Add(FIntPoint(X, Y));
				Visited[StartIndex] = 1;
				for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
				{
					const FIntPoint Cell = Queue[QueueIndex];
					Component.Cells.Add(Cell);
					Component.Min.X = FMath::Min(Component.Min.X, Cell.X);
					Component.Min.Y = FMath::Min(Component.Min.Y, Cell.Y);
					Component.Max.X = FMath::Max(Component.Max.X, Cell.X);
					Component.Max.Y = FMath::Max(Component.Max.Y, Cell.Y);
					for (const FIntPoint Offset : NeighborOffsets)
					{
						const FIntPoint Next = Cell + Offset;
						if (IsSolid(Mask, Width, Height, Next.X, Next.Y) && Visited[ToIndex(Width, Next.X, Next.Y)] == 0)
						{
							Visited[ToIndex(Width, Next.X, Next.Y)] = 1;
							Queue.Add(Next);
						}
					}
				}
				Component.Cells.Sort([](const FIntPoint& A, const FIntPoint& B) { return LexicographicLess(A, B); });
				OutComponents.Add(MoveTemp(Component));
			}
		}
	}

	bool BuildSupportAnchorMask(
		const TArray<uint8>& InitialMask,
		const int32 Width,
		const int32 Height,
		const EFragmentSupportMode SupportMode,
		TArray<uint8>& OutAnchorMask)
	{
		OutAnchorMask.Reset();
		if (!HasValidMaskDimensions(InitialMask, Width, Height))
		{
			return false;
		}
		for (const uint8 Cell : InitialMask)
		{
			if (Cell > 1)
			{
				return false;
			}
		}

		OutAnchorMask.Init(0, InitialMask.Num());
		if (SupportMode == EFragmentSupportMode::None)
		{
			return true;
		}
		if (SupportMode != EFragmentSupportMode::Bottom)
		{
			OutAnchorMask.Reset();
			return false;
		}

		int32 LowestOccupiedY = Height;
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				if (IsSolid(InitialMask, Width, Height, X, Y))
				{
					LowestOccupiedY = Y;
					break;
				}
			}
			if (LowestOccupiedY != Height)
			{
				break;
			}
		}
		if (LowestOccupiedY == Height)
		{
			OutAnchorMask.Reset();
			return false;
		}

		for (int32 X = 0; X < Width; ++X)
		{
			const int32 Index = ToIndex(Width, X, LowestOccupiedY);
			OutAnchorMask[Index] = InitialMask[Index] != 0 ? 1 : 0;
		}
		return true;
	}

	bool ClassifyMaskBySupport(
		const TArray<uint8>& CandidateMask,
		const TArray<uint8>& AnchorMask,
		const int32 Width,
		const int32 Height,
		const EFragmentSupportMode SupportMode,
		FFragmentSupportResult& OutResult)
	{
		OutResult = FFragmentSupportResult();
		if (!HasValidMaskDimensions(CandidateMask, Width, Height)
			|| AnchorMask.Num() != CandidateMask.Num())
		{
			return false;
		}
		for (const uint8 Cell : CandidateMask)
		{
			if (Cell > 1)
			{
				return false;
			}
		}
		for (const uint8 Cell : AnchorMask)
		{
			if (Cell > 1)
			{
				return false;
			}
		}
		if (SupportMode != EFragmentSupportMode::None
			&& SupportMode != EFragmentSupportMode::Bottom)
		{
			return false;
		}

		OutResult.SupportedMask.Init(0, CandidateMask.Num());
		TArray<FFragmentComponent> Components;
		ExtractConnectedComponents(
			CandidateMask,
			Width,
			Height,
			Components);
		for (FFragmentComponent& Component : Components)
		{
			bool bSupported = false;
			if (SupportMode == EFragmentSupportMode::Bottom)
			{
				for (const FIntPoint& Cell : Component.Cells)
				{
					if (AnchorMask[ToIndex(Width, Cell.X, Cell.Y)] != 0)
					{
						bSupported = true;
						break;
					}
				}
			}

			if (bSupported)
			{
				for (const FIntPoint& Cell : Component.Cells)
				{
					OutResult.SupportedMask[
						ToIndex(Width, Cell.X, Cell.Y)] = 1;
				}
			}
			else
			{
				OutResult.DetachedComponents.Add(MoveTemp(Component));
			}
		}
		return true;
	}

	bool BuildFragmentGeometryFromMask(const TArray<uint8>& Mask, const int32 Width, const int32 Height, const float CellSize, FFragmentGeometry2D& OutGeometry)
	{
		OutGeometry = FFragmentGeometry2D();
		if (!HasValidMaskDimensions(Mask, Width, Height) || !FMath::IsFinite(CellSize) || CellSize <= 0.0f) return false;
		if (!Mask.ContainsByPredicate([](const uint8 Cell) { return Cell != 0; })) return true;

		FFragmentGeometry2D Geometry;
		TArray<TArray<FIntPoint>> GridLoops;
		if (!ExtractBoundaryLoops(Mask, Width, Height, GridLoops)) return false;
		for (TArray<FIntPoint>& GridLoop : GridLoops)
		{
			const bool bOuter = SignedArea(GridLoop) > 0.0;
			SimplifyAndCanonicalize(GridLoop, bOuter);
			if (GridLoop.Num() < 3) return false;
			FFragmentContour Contour;
			for (const FIntPoint& Point : GridLoop) Contour.Vertices.Add(CellEdgeToLocal2D(Width, Height, CellSize, Point));
			(bOuter ? Geometry.OuterContours : Geometry.HoleContours).Add(MoveTemp(Contour));
		}
		auto SortContours = [](TArray<FFragmentContour>& Contours)
		{
			Contours.Sort([](const FFragmentContour& A, const FFragmentContour& B)
			{
				if (A.Vertices[0] != B.Vertices[0]) return LexicographicLess(A.Vertices[0], B.Vertices[0]);
				return FMath::Abs(ContourArea(A)) > FMath::Abs(ContourArea(B));
			});
		};
		SortContours(Geometry.OuterContours);
		SortContours(Geometry.HoleContours);
		if (Geometry.OuterContours.Num() == 0) return false;

		TArray<TArray<int32>> HolesByOuter;
		HolesByOuter.SetNum(Geometry.OuterContours.Num());
		for (int32 HoleIndex = 0; HoleIndex < Geometry.HoleContours.Num(); ++HoleIndex)
		{
			int32 BestOuter = INDEX_NONE;
			double BestArea = TNumericLimits<double>::Max();
			for (int32 OuterIndex = 0; OuterIndex < Geometry.OuterContours.Num(); ++OuterIndex)
			{
				if (PointInLoop(Geometry.HoleContours[HoleIndex].Vertices[0], Geometry.OuterContours[OuterIndex]))
				{
					const double Area = FMath::Abs(ContourArea(Geometry.OuterContours[OuterIndex]));
					if (Area < BestArea) { BestArea = Area; BestOuter = OuterIndex; }
				}
			}
			if (BestOuter == INDEX_NONE) return false;
			HolesByOuter[BestOuter].Add(HoleIndex);
		}

		for (int32 OuterIndex = 0; OuterIndex < Geometry.OuterContours.Num(); ++OuterIndex)
		{
			TArray<UE::Math::TVector2<double>> OuterVertices;
			for (const FVector2D& Point : Geometry.OuterContours[OuterIndex].Vertices) OuterVertices.Emplace(Point.X, Point.Y);
			UE::Geometry::TGeneralPolygon2<double> Polygon(UE::Geometry::TPolygon2<double>(MoveTemp(OuterVertices)));
			for (const int32 HoleIndex : HolesByOuter[OuterIndex])
			{
				TArray<UE::Math::TVector2<double>> HoleVertices;
				for (const FVector2D& Point : Geometry.HoleContours[HoleIndex].Vertices) HoleVertices.Emplace(Point.X, Point.Y);
				if (!Polygon.AddHole(UE::Geometry::TPolygon2<double>(MoveTemp(HoleVertices)), false, true)) return false;
			}
			UE::Geometry::FConstrainedDelaunay2d Triangulation;
			Triangulation.FillRule = UE::Geometry::FConstrainedDelaunay2d::EFillRule::Positive;
			Triangulation.bOutputCCW = true;
			Triangulation.Add(Polygon);
			if (!Triangulation.Triangulate() || Triangulation.Triangles.Num() == 0) return false;

			TArray<UE::Math::TVector2<double>> Vertices = MoveTemp(Triangulation.Vertices);
			TArray<UE::Geometry::FIndex3i> Triangles = MoveTemp(Triangulation.Triangles);
			const int32 VertexOffset = Geometry.Vertices2D.Num();
			for (const UE::Math::TVector2<double>& Vertex : Vertices) Geometry.Vertices2D.Emplace(Vertex.X, Vertex.Y);
			for (const UE::Geometry::FIndex3i& Triangle : Triangles)
			{
				if (!Vertices.IsValidIndex(Triangle.A) || !Vertices.IsValidIndex(Triangle.B) || !Vertices.IsValidIndex(Triangle.C)
					|| Triangle.A == Triangle.B || Triangle.B == Triangle.C || Triangle.C == Triangle.A)
				{
					return false;
				}
				const UE::Math::TVector2<double>& A = Vertices[Triangle.A];
				const UE::Math::TVector2<double>& B = Vertices[Triangle.B];
				const UE::Math::TVector2<double>& C = Vertices[Triangle.C];
				const double SignedTwiceArea = (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
				if (!FMath::IsFinite(SignedTwiceArea) || FMath::Abs(SignedTwiceArea) <= UE_SMALL_NUMBER)
				{
					return false;
				}
				Geometry.TriangleIndices.Append({ VertexOffset + Triangle.A, VertexOffset + Triangle.B, VertexOffset + Triangle.C });
			}
		}
		CanonicalizeTrianglesAndVertices(Geometry);
		for (const FFragmentContour& Outer : Geometry.OuterContours)
		{
			FFragmentContour& Hull = Geometry.CollisionContours.AddDefaulted_GetRef();
			BuildConvexHull(Outer.Vertices, Hull);
		}
		if (Geometry.TriangleIndices.Num() < 3) return false;
		OutGeometry = MoveTemp(Geometry);
		return true;
	}

	bool IsSpawnPayloadWithinReplicationBudget(const FFragmentSpawnPayload& Payload)
	{
		if (Payload.Vertices2D.Num() > MaximumReplicatedFaceVertices
			|| Payload.TriangleIndices.Num() > MaximumReplicatedTriangleIndices
			|| Payload.OuterContours.Num() > MaximumReplicatedContours
			|| Payload.HoleContours.Num() > MaximumReplicatedContours
			|| Payload.CollisionContours.Num() > MaximumReplicatedContours)
		{
			return false;
		}
		const bool bHasDetachedVoxelMask =
			Payload.DetachedVoxelMask.Width != 0
			|| Payload.DetachedVoxelMask.Height != 0
			|| !Payload.DetachedVoxelMask.SolidMask.IsEmpty();
		if (bHasDetachedVoxelMask
			&& (!Payload.DetachedVoxelMask.IsValid()
				|| Payload.DetachedVoxelMask.GeometryStyle
					!= EFragmentSourceGeometryStyle::VoxelBlocks))
		{
			return false;
		}

		int64 ContourVertexCount = 0;
		auto AccumulateContours = [&ContourVertexCount](const TArray<FFragmentContour>& Contours)
		{
			for (const FFragmentContour& Contour : Contours)
			{
				if (Contour.Vertices.Num() > MaximumReplicatedVerticesPerContour)
				{
					return false;
				}
				ContourVertexCount += Contour.Vertices.Num();
				if (ContourVertexCount > MaximumReplicatedContourVertices)
				{
					return false;
				}
			}
			return true;
		};
		return AccumulateContours(Payload.OuterContours)
			&& AccumulateContours(Payload.HoleContours)
			&& AccumulateContours(Payload.CollisionContours);
	}

	bool BuildSpawnPayloadsFromComponents(
		const TArray<FFragmentComponent>& Components, const FGuid& SourceId, const FTransform& SourceTransform,
		const int32 MaskWidth, const int32 MaskHeight, const int32 Revision, const float CellSize,
		const int32 MinAreaPixels, const int32 MaxFragments, const FVector& DamageCenterWorld,
		const float DamagePower, const int32 EventSeed,
		TArray<FFragmentSpawnPayload>& OutPayloads,
		const EFragmentSourceGeometryStyle GeometryStyle)
	{
		OutPayloads.Reset();
		if (!SourceId.IsValid() || !SourceTransform.IsValid() || MaskWidth <= 0 || MaskHeight <= 0
			|| static_cast<int64>(MaskWidth) * static_cast<int64>(MaskHeight) > MAX_int32
			|| !FMath::IsFinite(CellSize) || CellSize <= 0.0f || !FMath::IsFinite(DamagePower) || DamagePower < 0.0f
			|| (GeometryStyle != EFragmentSourceGeometryStyle::ExtrudedMask
				&& GeometryStyle != EFragmentSourceGeometryStyle::RadialColumn
				&& GeometryStyle != EFragmentSourceGeometryStyle::VoxelBlocks)
			|| !FMath::IsFinite(DamageCenterWorld.X) || !FMath::IsFinite(DamageCenterWorld.Y) || !FMath::IsFinite(DamageCenterWorld.Z)) return false;

		struct FValidatedComponent
		{
			TArray<FIntPoint> Cells;
			FIntPoint Min = FIntPoint(MAX_int32, MAX_int32);
			FIntPoint Max = FIntPoint(MIN_int32, MIN_int32);
		};
		TArray<FValidatedComponent> SortedComponents;
		TArray<FValidatedComponent> TinyComponents;
		const int32 MinimumArea = FMath::Max(MinAreaPixels, 1);
		for (const FFragmentComponent& Component : Components)
		{
			if (Component.Cells.IsEmpty()) return false;
			FValidatedComponent Validated;
			Validated.Cells = Component.Cells;
			Validated.Cells.Sort([](const FIntPoint& A, const FIntPoint& B) { return LexicographicLess(A, B); });
			for (int32 CellIndex = 0; CellIndex < Validated.Cells.Num(); ++CellIndex)
			{
				const FIntPoint& Cell = Validated.Cells[CellIndex];
				if (Cell.X < 0 || Cell.Y < 0 || Cell.X >= MaskWidth || Cell.Y >= MaskHeight
					|| (CellIndex > 0 && Cell == Validated.Cells[CellIndex - 1])) return false;
				Validated.Min.X = FMath::Min(Validated.Min.X, Cell.X);
				Validated.Min.Y = FMath::Min(Validated.Min.Y, Cell.Y);
				Validated.Max.X = FMath::Max(Validated.Max.X, Cell.X);
				Validated.Max.Y = FMath::Max(Validated.Max.Y, Cell.Y);
			}
			if (Validated.Cells.Num() < MinimumArea)
			{
				TinyComponents.Add(MoveTemp(Validated));
			}
			else
			{
				SortedComponents.Add(MoveTemp(Validated));
			}
		}
		const auto ComponentLess = [](const FValidatedComponent& A, const FValidatedComponent& B)
		{
			if (A.Cells.Num() != B.Cells.Num()) return A.Cells.Num() > B.Cells.Num();
			if (A.Min != B.Min) return LexicographicLess(A.Min, B.Min);
			if (A.Max != B.Max) return LexicographicLess(A.Max, B.Max);
			for (int32 CellIndex = 0; CellIndex < A.Cells.Num(); ++CellIndex)
			{
				if (A.Cells[CellIndex] != B.Cells[CellIndex])
				{
					return LexicographicLess(A.Cells[CellIndex], B.Cells[CellIndex]);
				}
			}
			return false;
		};
		SortedComponents.Sort(ComponentLess);
		TinyComponents.Sort(ComponentLess);
		const int32 KeepCount = FMath::Min3(
			FMath::Max(MaxFragments, 1),
			MaximumFragmentCount,
			SortedComponents.Num());
		TArray<FFragmentSpawnPayload> BuiltPayloads;
		BuiltPayloads.Reserve(FMath::Min(KeepCount + 1, MaximumFragmentCount));
		const auto BuildPayload = [&](const FValidatedComponent& Component,
			const int32 ComponentIndex, const float FadeOutDuration)
		{
			TArray<uint8> ComponentMask;
			ComponentMask.Init(0, MaskWidth * MaskHeight);
			for (const FIntPoint& Cell : Component.Cells)
			{
				ComponentMask[ToIndex(MaskWidth, Cell.X, Cell.Y)] = 1;
			}
			FFragmentGeometry2D Geometry;
			if (!BuildFragmentGeometryFromMask(ComponentMask, MaskWidth, MaskHeight, CellSize, Geometry)) return false;
			const FVector2D LocalMin = CellEdgeToLocal2D(MaskWidth, MaskHeight, CellSize, Component.Min);
			const FVector2D LocalMax = CellEdgeToLocal2D(MaskWidth, MaskHeight, CellSize, Component.Max + FIntPoint(1, 1));
			const FVector2D LocalCenter = (LocalMin + LocalMax) * 0.5f;
			auto TranslateContours = [&LocalCenter](TArray<FFragmentContour>& Contours)
			{
				for (FFragmentContour& Contour : Contours) for (FVector2D& Vertex : Contour.Vertices) Vertex -= LocalCenter;
			};
			for (FVector2D& Vertex : Geometry.Vertices2D) Vertex -= LocalCenter;
			TranslateContours(Geometry.OuterContours);
			TranslateContours(Geometry.HoleContours);
			TranslateContours(Geometry.CollisionContours);

			uint32 CellSizeBits = 0;
			static_assert(sizeof(CellSizeBits) == sizeof(CellSize));
			FMemory::Memcpy(&CellSizeBits, &CellSize, sizeof(CellSizeBits));
			FString Signature = FString::Printf(
				TEXT("%s|R%d|E%d|W%d|H%d|S%08X"),
				*SourceId.ToString(EGuidFormats::Digits), Revision, EventSeed, MaskWidth, MaskHeight, CellSizeBits);
			for (const FIntPoint& Cell : Component.Cells)
			{
				Signature += FString::Printf(TEXT("|C%d,%d"), Cell.X, Cell.Y);
			}
			FFragmentSpawnPayload Payload;
			Payload.FragmentId = FGuid::NewDeterministicGuid(Signature, static_cast<uint64>(static_cast<uint32>(EventSeed)));
			Payload.Revision = Revision;
			Payload.Vertices2D = MoveTemp(Geometry.Vertices2D);
			Payload.TriangleIndices = MoveTemp(Geometry.TriangleIndices);
			Payload.OuterContours = MoveTemp(Geometry.OuterContours);
			Payload.HoleContours = MoveTemp(Geometry.HoleContours);
			Payload.CollisionContours = MoveTemp(Geometry.CollisionContours);
			if (GeometryStyle == EFragmentSourceGeometryStyle::VoxelBlocks)
			{
				FFragmentSourceMask& VoxelMask = Payload.DetachedVoxelMask;
				VoxelMask.Width = Component.Max.X - Component.Min.X + 1;
				VoxelMask.Height = Component.Max.Y - Component.Min.Y + 1;
				VoxelMask.CellSize = CellSize;
				VoxelMask.MinFragmentAreaPixels = 1;
				VoxelMask.MaxFragmentsPerBreak = MaximumFragmentCount;
				VoxelMask.SupportMode = EFragmentSupportMode::None;
				VoxelMask.GeometryStyle =
					EFragmentSourceGeometryStyle::VoxelBlocks;
				VoxelMask.SolidMask.Init(
					0,
					VoxelMask.Width * VoxelMask.Height);
				for (const FIntPoint& Cell : Component.Cells)
				{
					const int32 LocalX = Cell.X - Component.Min.X;
					const int32 LocalY = Cell.Y - Component.Min.Y;
					VoxelMask.SolidMask[
						LocalY * VoxelMask.Width + LocalX] = 1;
				}
			}
			Payload.FadeOutDuration = FadeOutDuration;
			Payload.bEnableCollision = FadeOutDuration <= 0.0f;
			Payload.Thickness = CellSize;
			Payload.InitialTransform = FTransform(
				SourceTransform.GetRotation(),
				SourceTransform.TransformPosition(
					FVector(LocalCenter.X, 0.0f, LocalCenter.Y)),
				SourceTransform.GetScale3D());
			Payload.Mass = FMath::Clamp(static_cast<float>(Component.Cells.Num()) * 0.05f, 0.5f, 50.0f);
			FRandomStream Random(HashCombineFast(EventSeed, ComponentIndex));
			const FVector FragmentCenterWorld = Payload.InitialTransform.GetLocation();
			FVector Direction = FragmentCenterWorld - DamageCenterWorld;
			if (!Direction.Normalize()) Direction = FVector(Random.FRandRange(-1.0f, 1.0f), 0.0f, Random.FRandRange(0.25f, 1.0f)).GetSafeNormal();
			const float Distance = FVector::Distance(FragmentCenterWorld, DamageCenterWorld);
			const float Falloff = 1.0f / FMath::Max(1.0f, Distance / FMath::Max(CellSize, 1.0f));
			Payload.InitialLinearVelocity = Direction * DamagePower * Falloff / FMath::Max(Payload.Mass, 0.5f);
			Payload.InitialAngularVelocity = DamagePower > 0.0f
				? FVector(0.0f, Random.FRandRange(-180.0f, 180.0f), 0.0f)
				: FVector::ZeroVector;
			if (Payload.InitialTransform.ContainsNaN()
				|| Payload.InitialLinearVelocity.ContainsNaN()
				|| Payload.InitialAngularVelocity.ContainsNaN()
				|| !FMath::IsFinite(Payload.Mass)
				|| Payload.Mass <= 0.0f
				|| !IsSpawnPayloadWithinReplicationBudget(Payload))
			{
				return false;
			}
			BuiltPayloads.Add(MoveTemp(Payload));
			return true;
		};
		for (int32 ComponentIndex = 0; ComponentIndex < KeepCount; ++ComponentIndex)
		{
			if (!BuildPayload(SortedComponents[ComponentIndex], ComponentIndex, 0.0f))
			{
				return false;
			}
		}

		// Sub-threshold debris gets one short-lived render carrier per damaged
		// source. Keeping this bounded avoids replacing one disappearing pixel
		// with a burst of tiny replicated Actors.
		if (!TinyComponents.IsEmpty()
			&& BuiltPayloads.Num() < FMath::Min(
				FMath::Max(MaxFragments, 1),
				MaximumFragmentCount))
		{
			constexpr int32 MaximumFadingComponents = 64;
			constexpr int32 MaximumFadingCells = 192;
			FValidatedComponent Combined;
			int32 CombinedComponentCount = 0;
			for (const FValidatedComponent& Tiny : TinyComponents)
			{
				if (Combined.Cells.Num() + Tiny.Cells.Num() > MaximumFadingCells
					|| CombinedComponentCount >= MaximumFadingComponents)
				{
					break;
				}
				Combined.Cells.Append(Tiny.Cells);
				++CombinedComponentCount;
				Combined.Min.X = FMath::Min(Combined.Min.X, Tiny.Min.X);
				Combined.Min.Y = FMath::Min(Combined.Min.Y, Tiny.Min.Y);
				Combined.Max.X = FMath::Max(Combined.Max.X, Tiny.Max.X);
				Combined.Max.Y = FMath::Max(Combined.Max.Y, Tiny.Max.Y);
			}
			Combined.Cells.Sort([](const FIntPoint& A, const FIntPoint& B)
			{
				return LexicographicLess(A, B);
			});
			for (int32 Index = 1; Index < Combined.Cells.Num(); ++Index)
			{
				if (Combined.Cells[Index] == Combined.Cells[Index - 1])
				{
					return false;
				}
			}
			if (!Combined.Cells.IsEmpty()
				&& !BuildPayload(Combined, KeepCount, 0.45f))
			{
				return false;
			}
		}
		OutPayloads = MoveTemp(BuiltPayloads);
		return true;
	}

	bool BuildExtrudedMesh(
		const TArray<FVector2D>& Vertices2D, const TArray<int32>& TriangleIndices,
		const TArray<FFragmentContour>& OuterContours, const TArray<FFragmentContour>& HoleContours,
		const float Thickness, TArray<FVector>& OutVertices, TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals, TArray<FVector2D>& OutUVs)
	{
		auto ResetOutputs = [&OutVertices, &OutTriangles, &OutNormals, &OutUVs]()
		{
			OutVertices.Reset();
			OutTriangles.Reset();
			OutNormals.Reset();
			OutUVs.Reset();
		};
		ResetOutputs();
		if (Vertices2D.Num() < 3 || TriangleIndices.Num() == 0 || TriangleIndices.Num() % 3 != 0
			|| OuterContours.Num() == 0 || !FMath::IsFinite(Thickness) || Thickness <= 0.0f) return false;
		for (const FVector2D& Vertex : Vertices2D)
		{
			if (!FMath::IsFinite(Vertex.X) || !FMath::IsFinite(Vertex.Y)) return false;
		}
		for (int32 TriangleIndex = 0; TriangleIndex < TriangleIndices.Num(); TriangleIndex += 3)
		{
			const int32 A = TriangleIndices[TriangleIndex];
			const int32 B = TriangleIndices[TriangleIndex + 1];
			const int32 C = TriangleIndices[TriangleIndex + 2];
			if (!Vertices2D.IsValidIndex(A) || !Vertices2D.IsValidIndex(B) || !Vertices2D.IsValidIndex(C)
				|| A == B || B == C || C == A)
			{
				return false;
			}
			const double SignedTwiceArea = FVector2D::CrossProduct(
				Vertices2D[B] - Vertices2D[A],
				Vertices2D[C] - Vertices2D[A]);
			if (!FMath::IsFinite(SignedTwiceArea) || SignedTwiceArea <= UE_SMALL_NUMBER)
			{
				return false;
			}
		}
		const float HalfThickness = Thickness * 0.5f;
		for (const FVector2D& Vertex : Vertices2D)
		{
			OutVertices.Add(FVector(Vertex.X, HalfThickness, Vertex.Y)); OutNormals.Add(FVector::YAxisVector); OutUVs.Add(Vertex * 0.01f);
		}
		const int32 BackOffset = OutVertices.Num();
		for (const FVector2D& Vertex : Vertices2D)
		{
			OutVertices.Add(FVector(Vertex.X, -HalfThickness, Vertex.Y)); OutNormals.Add(-FVector::YAxisVector); OutUVs.Add(Vertex * 0.01f);
		}
		for (int32 Index = 0; Index < TriangleIndices.Num(); Index += 3)
		{
			const int32 A = TriangleIndices[Index], B = TriangleIndices[Index + 1], C = TriangleIndices[Index + 2];
			OutTriangles.Append({ A, C, B, A + BackOffset, B + BackOffset, C + BackOffset });
		}
		auto AddSides = [&](const TArray<FFragmentContour>& Contours, const bool bWantCounterClockwise)
		{
			for (const FFragmentContour& Contour : Contours)
			{
				if (Contour.Vertices.Num() < 3) return false;
				const double Area = ContourArea(Contour);
				if (!FMath::IsFinite(Area) || FMath::Abs(Area) <= UE_SMALL_NUMBER
					|| (Area > 0.0) != bWantCounterClockwise) return false;
				for (int32 Index = 0; Index < Contour.Vertices.Num(); ++Index)
				{
					const FVector2D& A = Contour.Vertices[Index];
					const FVector2D& B = Contour.Vertices[(Index + 1) % Contour.Vertices.Num()];
					if (!FMath::IsFinite(A.X) || !FMath::IsFinite(A.Y) || !FMath::IsFinite(B.X) || !FMath::IsFinite(B.Y)) return false;
					const FVector2D Edge = B - A;
					if (Edge.IsNearlyZero()) return false;
					const FVector Normal = FVector(Edge.Y, 0.0f, -Edge.X).GetSafeNormal();
					const float EdgeLength = Edge.Length();
					const int32 Base = OutVertices.Num();
					OutVertices.Append({ FVector(A.X, HalfThickness, A.Y), FVector(B.X, HalfThickness, B.Y), FVector(B.X, -HalfThickness, B.Y), FVector(A.X, -HalfThickness, A.Y) });
					OutNormals.Append({ Normal, Normal, Normal, Normal });
					OutUVs.Append({ FVector2D(0.0f, 0.0f), FVector2D(EdgeLength * 0.01f, 0.0f), FVector2D(EdgeLength * 0.01f, Thickness * 0.01f), FVector2D(0.0f, Thickness * 0.01f) });
					OutTriangles.Append({ Base, Base + 1, Base + 2, Base, Base + 2, Base + 3 });
				}
			}
			return true;
		};
		if (!AddSides(OuterContours, true) || !AddSides(HoleContours, false))
		{
			ResetOutputs();
			return false;
		}
		for (const int32 Index : OutTriangles)
		{
			if (!OutVertices.IsValidIndex(Index))
			{
				ResetOutputs();
				return false;
			}
		}
		if (OutVertices.Num() != OutNormals.Num() || OutVertices.Num() != OutUVs.Num())
		{
			ResetOutputs();
			return false;
		}
		return true;
	}

	bool BuildVoxelBlockMeshFromCells(
		const TArray<FIntVector>& SolidCells,
		const float CellSize,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals,
		TArray<FVector2D>& OutUVs,
		int32& OutPrimaryIndexCount)
	{
		return BuildVoxelBlockMeshFromCellsWithOccluders(
			SolidCells,
			{},
			CellSize,
			OutVertices,
			OutTriangles,
			OutNormals,
			OutUVs,
			OutPrimaryIndexCount);
	}

	bool BuildVoxelBlockMeshFromCellsWithOccluders(
		const TArray<FIntVector>& SurfaceCells,
		const TArray<FIntVector>& OccluderCells,
		const float CellSize,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals,
		TArray<FVector2D>& OutUVs,
		int32& OutPrimaryIndexCount)
	{
		OutVertices.Reset();
		OutTriangles.Reset();
		OutNormals.Reset();
		OutUVs.Reset();
		OutPrimaryIndexCount = 0;
		if (SurfaceCells.IsEmpty()
			|| SurfaceCells.Num() > 1024 * 1024
			|| OccluderCells.Num() > 1024 * 1024
			|| static_cast<int64>(SurfaceCells.Num()) + OccluderCells.Num()
				> 1024 * 1024
			|| !FMath::IsFinite(CellSize)
			|| CellSize <= 0.0f)
		{
			return false;
		}

		TSet<FIntVector> OccupiedCells;
		OccupiedCells.Reserve(SurfaceCells.Num() + OccluderCells.Num());
		for (const FIntVector& Cell : SurfaceCells)
		{
			OccupiedCells.Add(Cell);
		}
		for (const FIntVector& Cell : OccluderCells)
		{
			OccupiedCells.Add(Cell);
		}
		TSet<FIntVector> UniqueSurfaceCells;
		UniqueSurfaceCells.Reserve(SurfaceCells.Num());
		for (const FIntVector& Cell : SurfaceCells)
		{
			UniqueSurfaceCells.Add(Cell);
		}
		TArray<FIntVector> OrderedCells = UniqueSurfaceCells.Array();
		OrderedCells.Sort([](const FIntVector& Left, const FIntVector& Right)
		{
			if (Left.Z != Right.Z)
			{
				return Left.Z < Right.Z;
			}
			if (Left.Y != Right.Y)
			{
				return Left.Y < Right.Y;
			}
			return Left.X < Right.X;
		});

		TArray<int32> PrimaryTriangles;
		TArray<int32> SideTriangles;
		OutVertices.Reserve(OrderedCells.Num() * 24);
		OutNormals.Reserve(OrderedCells.Num() * 24);
		OutUVs.Reserve(OrderedCells.Num() * 24);
		PrimaryTriangles.Reserve(OrderedCells.Num() * 12);
		SideTriangles.Reserve(OrderedCells.Num() * 24);
		const float HalfBlock = CellSize * 0.5f;
		const TArray<FVector2D> QuadUVs = {
			FVector2D(0.0f, 0.0f), FVector2D(1.0f, 0.0f),
			FVector2D(1.0f, 1.0f), FVector2D(0.0f, 1.0f) };
		auto AddQuad = [&OutVertices, &OutNormals, &OutUVs, &QuadUVs](
			TArray<int32>& TargetTriangles,
			const FVector& P0,
			const FVector& P1,
			const FVector& P2,
			const FVector& P3,
			const FVector& Normal)
		{
			const int32 Base = OutVertices.Num();
			OutVertices.Append({P0, P1, P2, P3});
			OutNormals.Append({Normal, Normal, Normal, Normal});
			OutUVs.Append(QuadUVs);
			TargetTriangles.Append({
				Base, Base + 2, Base + 1,
				Base, Base + 3, Base + 2 });
		};

		for (const FIntVector& Cell : OrderedCells)
		{
			const FVector Center(Cell.X * CellSize, Cell.Y * CellSize,
				Cell.Z * CellSize);
			const float MinX = Center.X - HalfBlock;
			const float MaxX = Center.X + HalfBlock;
			const float MinY = Center.Y - HalfBlock;
			const float MaxY = Center.Y + HalfBlock;
			const float MinZ = Center.Z - HalfBlock;
			const float MaxZ = Center.Z + HalfBlock;
			if (!OccupiedCells.Contains(Cell + FIntVector(0, 1, 0)))
			{
				AddQuad(PrimaryTriangles,
					FVector(MinX, MaxY, MinZ), FVector(MinX, MaxY, MaxZ),
					FVector(MaxX, MaxY, MaxZ), FVector(MaxX, MaxY, MinZ),
					FVector::YAxisVector);
			}
			if (!OccupiedCells.Contains(Cell + FIntVector(0, -1, 0)))
			{
				AddQuad(PrimaryTriangles,
					FVector(MinX, MinY, MinZ), FVector(MaxX, MinY, MinZ),
					FVector(MaxX, MinY, MaxZ), FVector(MinX, MinY, MaxZ),
					-FVector::YAxisVector);
			}
			if (!OccupiedCells.Contains(Cell + FIntVector(1, 0, 0)))
			{
				AddQuad(SideTriangles,
					FVector(MaxX, MinY, MinZ), FVector(MaxX, MaxY, MinZ),
					FVector(MaxX, MaxY, MaxZ), FVector(MaxX, MinY, MaxZ),
					FVector::XAxisVector);
			}
			if (!OccupiedCells.Contains(Cell + FIntVector(-1, 0, 0)))
			{
				AddQuad(SideTriangles,
					FVector(MinX, MaxY, MinZ), FVector(MinX, MinY, MinZ),
					FVector(MinX, MinY, MaxZ), FVector(MinX, MaxY, MaxZ),
					-FVector::XAxisVector);
			}
			if (!OccupiedCells.Contains(Cell + FIntVector(0, 0, 1)))
			{
				AddQuad(SideTriangles,
					FVector(MinX, MinY, MaxZ), FVector(MaxX, MinY, MaxZ),
					FVector(MaxX, MaxY, MaxZ), FVector(MinX, MaxY, MaxZ),
					FVector::ZAxisVector);
			}
			if (!OccupiedCells.Contains(Cell + FIntVector(0, 0, -1)))
			{
				AddQuad(SideTriangles,
					FVector(MinX, MaxY, MinZ), FVector(MaxX, MaxY, MinZ),
					FVector(MaxX, MinY, MinZ), FVector(MinX, MinY, MinZ),
					-FVector::ZAxisVector);
			}
		}

		OutPrimaryIndexCount = PrimaryTriangles.Num();
		OutTriangles = MoveTemp(PrimaryTriangles);
		OutTriangles.Append(SideTriangles);
		if (OutTriangles.IsEmpty()
			|| OutVertices.Num() != OutNormals.Num()
			|| OutVertices.Num() != OutUVs.Num())
		{
			OutVertices.Reset();
			OutTriangles.Reset();
			OutNormals.Reset();
			OutUVs.Reset();
			OutPrimaryIndexCount = 0;
			return false;
		}
		return true;
	}

	bool BuildVoxelBlockMeshFromMask(
		const TArray<uint8>& Mask,
		const int32 Width,
		const int32 Height,
		const float CellSize,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals,
		TArray<FVector2D>& OutUVs,
		int32& OutPrimaryIndexCount)
	{
		OutVertices.Reset();
		OutTriangles.Reset();
		OutNormals.Reset();
		OutUVs.Reset();
		OutPrimaryIndexCount = 0;
		const int64 CellCount = static_cast<int64>(Width) * Height;
		if (Width <= 0 || Height <= 0 || Width > 256 || Height > 256
			|| CellCount != Mask.Num()
			|| !FMath::IsFinite(CellSize) || CellSize <= 0.0f)
		{
			return false;
		}
		int32 SolidCount = 0;
		for (const uint8 Cell : Mask)
		{
			if (Cell > 1)
			{
				return false;
			}
			SolidCount += Cell != 0 ? 1 : 0;
		}
		if (SolidCount == 0)
		{
			return false;
		}

		OutVertices.Reserve(SolidCount * 24);
		OutNormals.Reserve(SolidCount * 24);
		OutUVs.Reserve(SolidCount * 24);
		TArray<int32> PrimaryTriangles;
		TArray<int32> SideTriangles;
		PrimaryTriangles.Reserve(SolidCount * 12);
		SideTriangles.Reserve(SolidCount * 24);
		// 相邻体素必须严丝合缝。块间人为留缝会让树冠读成珠串，
		// 叶片的细碎边缘由专用 masked 像素材质负责。
		const float HalfBlock = CellSize * 0.5f;
		const TArray<FVector2D> QuadUVs = {
			FVector2D(0.0f, 0.0f), FVector2D(1.0f, 0.0f),
			FVector2D(1.0f, 1.0f), FVector2D(0.0f, 1.0f) };
		auto AddQuad = [&OutVertices, &OutNormals, &OutUVs, &QuadUVs](
			TArray<int32>& TargetTriangles,
			const FVector& P0,
			const FVector& P1,
			const FVector& P2,
			const FVector& P3,
			const FVector& Normal)
		{
			const int32 Base = OutVertices.Num();
			OutVertices.Append({ P0, P1, P2, P3 });
			OutNormals.Append({ Normal, Normal, Normal, Normal });
			OutUVs.Append(QuadUVs);
			TargetTriangles.Append({
				Base, Base + 2, Base + 1,
				Base, Base + 3, Base + 2 });
		};

		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				if (Mask[Y * Width + X] == 0)
				{
					continue;
				}
				const float CenterX =
					(static_cast<float>(X) + 0.5f
						- static_cast<float>(Width) * 0.5f) * CellSize;
				const float CenterZ =
					(static_cast<float>(Y) + 0.5f
						- static_cast<float>(Height) * 0.5f) * CellSize;
				const float MinX = CenterX - HalfBlock;
				const float MaxX = CenterX + HalfBlock;
				const float MinY = -HalfBlock;
				const float MaxY = HalfBlock;
				const float MinZ = CenterZ - HalfBlock;
				const float MaxZ = CenterZ + HalfBlock;

				// 正反面先写入 primary 索引，保持现有 face/side 材质分区。
				AddQuad(
					PrimaryTriangles,
					FVector(MinX, MaxY, MinZ), FVector(MinX, MaxY, MaxZ),
					FVector(MaxX, MaxY, MaxZ), FVector(MaxX, MaxY, MinZ),
					FVector::YAxisVector);
				AddQuad(
					PrimaryTriangles,
					FVector(MinX, MinY, MinZ), FVector(MaxX, MinY, MinZ),
					FVector(MaxX, MinY, MaxZ), FVector(MinX, MinY, MaxZ),
					-FVector::YAxisVector);
				// 同一切片内相邻方块共享的面必须剔除。保留这些内部面会
				// 在 masked 叶片孔洞后重复绘制，并在块边界产生亮缝。
				if (X + 1 >= Width || Mask[Y * Width + X + 1] == 0)
				{
					AddQuad(
						SideTriangles,
						FVector(MaxX, MinY, MinZ), FVector(MaxX, MaxY, MinZ),
						FVector(MaxX, MaxY, MaxZ), FVector(MaxX, MinY, MaxZ),
						FVector::XAxisVector);
				}
				if (X == 0 || Mask[Y * Width + X - 1] == 0)
				{
					AddQuad(
						SideTriangles,
						FVector(MinX, MaxY, MinZ), FVector(MinX, MinY, MinZ),
						FVector(MinX, MinY, MaxZ), FVector(MinX, MaxY, MaxZ),
						-FVector::XAxisVector);
				}
				if (Y + 1 >= Height || Mask[(Y + 1) * Width + X] == 0)
				{
					AddQuad(
						SideTriangles,
						FVector(MinX, MinY, MaxZ), FVector(MaxX, MinY, MaxZ),
						FVector(MaxX, MaxY, MaxZ), FVector(MinX, MaxY, MaxZ),
						FVector::ZAxisVector);
				}
				if (Y == 0 || Mask[(Y - 1) * Width + X] == 0)
				{
					AddQuad(
						SideTriangles,
						FVector(MinX, MaxY, MinZ), FVector(MaxX, MaxY, MinZ),
						FVector(MaxX, MinY, MinZ), FVector(MinX, MinY, MinZ),
						-FVector::ZAxisVector);
				}
			}
		}

		OutPrimaryIndexCount = PrimaryTriangles.Num();
		OutTriangles = MoveTemp(PrimaryTriangles);
		OutTriangles.Append(SideTriangles);
		if (OutPrimaryIndexCount <= 0
			|| OutPrimaryIndexCount >= OutTriangles.Num()
			|| OutVertices.Num() != OutNormals.Num()
			|| OutVertices.Num() != OutUVs.Num())
		{
			OutVertices.Reset();
			OutTriangles.Reset();
			OutNormals.Reset();
			OutUVs.Reset();
			OutPrimaryIndexCount = 0;
			return false;
		}
		for (const int32 Index : OutTriangles)
		{
			if (!OutVertices.IsValidIndex(Index))
			{
				OutVertices.Reset();
				OutTriangles.Reset();
				OutNormals.Reset();
				OutUVs.Reset();
				OutPrimaryIndexCount = 0;
				return false;
			}
		}
		return true;
	}

	bool GatherTopExposedMarkedMaskCells(
		const TArray<uint8>& OccupiedMask,
		const TArray<uint8>& MarkedMask,
		const int32 Width,
		const int32 Height,
		TArray<int32>& OutCellIndices)
	{
		OutCellIndices.Reset();
		const int64 CellCount = static_cast<int64>(Width) * Height;
		if (Width <= 0 || Height <= 0
			|| CellCount != OccupiedMask.Num()
			|| CellCount != MarkedMask.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < MarkedMask.Num(); ++Index)
		{
			if (MarkedMask[Index] == 0 || OccupiedMask[Index] == 0)
			{
				continue;
			}
			const int32 Y = Index / Width;
			const int32 AboveIndex = Index + Width;
			if (Y + 1 >= Height || OccupiedMask[AboveIndex] == 0)
			{
				OutCellIndices.Add(Index);
			}
		}
		return true;
	}

	bool BuildRadialColumnMeshFromMask(
		const TArray<uint8>& Mask,
		const int32 Width,
		const int32 Height,
		const float CellSize,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals,
		TArray<FVector2D>& OutUVs,
		int32& OutPrimaryIndexCount)
	{
		OutVertices.Reset();
		OutTriangles.Reset();
		OutNormals.Reset();
		OutUVs.Reset();
		OutPrimaryIndexCount = 0;
		const int64 CellCount = static_cast<int64>(Width) * Height;
		if (Width <= 0 || Height <= 0 || Width > 256 || Height > 256
			|| CellCount != Mask.Num()
			|| !FMath::IsFinite(CellSize) || CellSize <= 0.0f)
		{
			return false;
		}
		for (const uint8 Cell : Mask)
		{
			if (Cell > 1)
			{
				return false;
			}
		}

		struct FColumnSpan
		{
			int32 MinX = 0;
			int32 MaxX = 0;
			int32 StartY = 0;
			int32 EndY = 0;
		};
		TMap<int32, FColumnSpan> ActiveSpans;
		TArray<FColumnSpan> CompletedSpans;
		for (int32 Y = 0; Y < Height; ++Y)
		{
			TSet<int32> RowKeys;
			for (int32 X = 0; X < Width;)
			{
				if (Mask[Y * Width + X] == 0)
				{
					++X;
					continue;
				}
				const int32 MinX = X;
				while (X + 1 < Width && Mask[Y * Width + X + 1] != 0)
				{
					++X;
				}
				const int32 MaxX = X++;
				const int32 Key = MinX * 256 + MaxX;
				RowKeys.Add(Key);
				if (FColumnSpan* Existing = ActiveSpans.Find(Key))
				{
					Existing->EndY = Y;
				}
				else
				{
					ActiveSpans.Add(Key, { MinX, MaxX, Y, Y });
				}
			}

			TArray<int32> FinishedKeys;
			for (const TPair<int32, FColumnSpan>& Pair : ActiveSpans)
			{
				if (!RowKeys.Contains(Pair.Key))
				{
					CompletedSpans.Add(Pair.Value);
					FinishedKeys.Add(Pair.Key);
				}
			}
			for (const int32 Key : FinishedKeys)
			{
				ActiveSpans.Remove(Key);
			}
		}
		for (const TPair<int32, FColumnSpan>& Pair : ActiveSpans)
		{
			CompletedSpans.Add(Pair.Value);
		}
		CompletedSpans.Sort([](const FColumnSpan& A, const FColumnSpan& B)
		{
			if (A.StartY != B.StartY) return A.StartY < B.StartY;
			if (A.MinX != B.MinX) return A.MinX < B.MinX;
			if (A.EndY != B.EndY) return A.EndY < B.EndY;
			return A.MaxX < B.MaxX;
		});
		if (CompletedSpans.IsEmpty())
		{
			return false;
		}

		constexpr int32 SideCount = 8;
		TArray<int32> PrimaryTriangles;
		TArray<int32> CapTriangles;
		for (const FColumnSpan& Span : CompletedSpans)
		{
			const float CenterX =
				(static_cast<float>(Span.MinX + Span.MaxX + 1) * 0.5f
					- static_cast<float>(Width) * 0.5f) * CellSize;
			const float Radius = FMath::Max(
				CellSize * 0.45f,
				static_cast<float>(Span.MaxX - Span.MinX + 1)
					* CellSize * 0.5f);
			const float BottomZ =
				(static_cast<float>(Span.StartY)
					- static_cast<float>(Height) * 0.5f) * CellSize;
			const float TopZ =
				(static_cast<float>(Span.EndY + 1)
					- static_cast<float>(Height) * 0.5f) * CellSize;
			const float HeightUv = (TopZ - BottomZ) * 0.01f;

			for (int32 Side = 0; Side < SideCount; ++Side)
			{
				const float A0 = 2.0f * PI * static_cast<float>(Side)
					/ static_cast<float>(SideCount);
				const float A1 = 2.0f * PI * static_cast<float>(Side + 1)
					/ static_cast<float>(SideCount);
				const FVector N0(FMath::Cos(A0), FMath::Sin(A0), 0.0f);
				const FVector N1(FMath::Cos(A1), FMath::Sin(A1), 0.0f);
				const FVector FacetNormal = (N0 + N1).GetSafeNormal();
				const FVector P0(CenterX + N0.X * Radius, N0.Y * Radius, BottomZ);
				const FVector P1(CenterX + N1.X * Radius, N1.Y * Radius, BottomZ);
				const FVector P2(CenterX + N1.X * Radius, N1.Y * Radius, TopZ);
				const FVector P3(CenterX + N0.X * Radius, N0.Y * Radius, TopZ);
				const int32 Base = OutVertices.Num();
				OutVertices.Append({ P0, P1, P2, P3 });
				OutNormals.Append({ FacetNormal, FacetNormal, FacetNormal, FacetNormal });
				OutUVs.Append({
					FVector2D(0.0f, 0.0f), FVector2D(1.0f, 0.0f),
					FVector2D(1.0f, HeightUv), FVector2D(0.0f, HeightUv) });
				PrimaryTriangles.Append({
					Base, Base + 1, Base + 2,
					Base, Base + 2, Base + 3 });

				const int32 CapBase = OutVertices.Num();
				OutVertices.Append({
					FVector(CenterX, 0.0f, BottomZ), P1, P0,
					FVector(CenterX, 0.0f, TopZ), P3, P2 });
				OutNormals.Append({
					-FVector::ZAxisVector, -FVector::ZAxisVector,
					-FVector::ZAxisVector, FVector::ZAxisVector,
					FVector::ZAxisVector, FVector::ZAxisVector });
				OutUVs.Append({
					FVector2D(0.5f, 0.5f), FVector2D(N1.X, N1.Y),
					FVector2D(N0.X, N0.Y), FVector2D(0.5f, 0.5f),
					FVector2D(N0.X, N0.Y), FVector2D(N1.X, N1.Y) });
				CapTriangles.Append({
					CapBase, CapBase + 1, CapBase + 2,
					CapBase + 3, CapBase + 4, CapBase + 5 });
			}
		}

		OutPrimaryIndexCount = PrimaryTriangles.Num();
		OutTriangles = MoveTemp(PrimaryTriangles);
		OutTriangles.Append(CapTriangles);
		if (OutPrimaryIndexCount <= 0
			|| OutPrimaryIndexCount >= OutTriangles.Num()
			|| OutVertices.Num() != OutNormals.Num()
			|| OutVertices.Num() != OutUVs.Num())
		{
			OutVertices.Reset();
			OutTriangles.Reset();
			OutNormals.Reset();
			OutUVs.Reset();
			OutPrimaryIndexCount = 0;
			return false;
		}
		return true;
	}
}
