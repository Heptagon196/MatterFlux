#include "Game/MatterFluxTerrainMesh.h"

namespace MatterFlux::TerrainMesh
{
	bool FSection::IsValid() const
	{
		if (Vertices.IsEmpty()
			|| Triangles.IsEmpty()
			|| Triangles.Num() % 3 != 0
			|| Normals.Num() != Vertices.Num()
			|| UVs.Num() != Vertices.Num())
		{
			return false;
		}
		for (const int32 Index : Triangles)
		{
			if (!Vertices.IsValidIndex(Index))
			{
				return false;
			}
		}
		for (const FVector& Vertex : Vertices)
		{
			if (Vertex.ContainsNaN())
			{
				return false;
			}
		}
		for (const FVector& Normal : Normals)
		{
			if (Normal.ContainsNaN())
			{
				return false;
			}
		}
		for (const FVector2D& UV : UVs)
		{
			if (UV.ContainsNaN())
			{
				return false;
			}
		}
		return true;
	}

	bool FChunk::IsValid() const
	{
		return CellBounds.Area() > 0
			&& !Sections.IsEmpty()
			&& CollisionSurface.IsValid()
			&& Sections.ContainsByPredicate(
				[](const FSection& Section)
				{
					return Section.IsValid();
				});
	}

	int32 FChunk::GetTriangleCount() const
	{
		int32 Count = 0;
		for (const FSection& Section : Sections)
		{
			Count += Section.Triangles.Num() / 3;
		}
		return Count;
	}

	bool BuildChunk(
		const PlayableLevel::FLevelTerrain& Terrain,
		const FIntPoint ChunkCoordinate,
		const int32 ChunkSize,
		FChunk& OutChunk)
	{
		OutChunk = FChunk();
		if (!Terrain.IsValid() || ChunkSize <= 0)
		{
			return false;
		}

		const double FirstWorldCellX =
			Terrain.FirstCellCenter.X / Terrain.CellSize;
		const double FirstWorldCellY =
			Terrain.FirstCellCenter.Y / Terrain.CellSize;
		if (FirstWorldCellX < MIN_int32
			|| FirstWorldCellX > MAX_int32
			|| FirstWorldCellY < MIN_int32
			|| FirstWorldCellY > MAX_int32)
		{
			return false;
		}
		const FIntPoint FirstWorldCell(
			FMath::FloorToInt(FirstWorldCellX),
			FMath::FloorToInt(FirstWorldCellY));
		const int64 ChunkWorldMinX =
			static_cast<int64>(ChunkCoordinate.X) * ChunkSize;
		const int64 ChunkWorldMinY =
			static_cast<int64>(ChunkCoordinate.Y) * ChunkSize;
		const int64 ChunkWorldMaxX =
			ChunkWorldMinX + ChunkSize;
		const int64 ChunkWorldMaxY =
			ChunkWorldMinY + ChunkSize;
		if (ChunkWorldMinX < MIN_int32
			|| ChunkWorldMinX > MAX_int32
			|| ChunkWorldMinY < MIN_int32
			|| ChunkWorldMinY > MAX_int32
			|| ChunkWorldMaxX < MIN_int32
			|| ChunkWorldMaxX > MAX_int32
			|| ChunkWorldMaxY < MIN_int32
			|| ChunkWorldMaxY > MAX_int32)
		{
			return false;
		}
		FIntPoint LocalMin = FIntPoint::ZeroValue;
		FIntPoint LocalMax(ChunkSize, ChunkSize);
		if (!Terrain.bInfinite)
		{
			LocalMin = FIntPoint(
				FMath::Clamp(
					static_cast<int64>(ChunkWorldMinX)
						- FirstWorldCell.X,
					static_cast<int64>(0),
					static_cast<int64>(Terrain.Width)),
				FMath::Clamp(
					static_cast<int64>(ChunkWorldMinY)
						- FirstWorldCell.Y,
					static_cast<int64>(0),
					static_cast<int64>(Terrain.Height)));
			LocalMax = FIntPoint(
				FMath::Clamp(
					static_cast<int64>(ChunkWorldMaxX)
						- FirstWorldCell.X,
					static_cast<int64>(0),
					static_cast<int64>(Terrain.Width)),
				FMath::Clamp(
					static_cast<int64>(ChunkWorldMaxY)
						- FirstWorldCell.Y,
					static_cast<int64>(0),
					static_cast<int64>(Terrain.Height)));
		}
		if (LocalMax.X <= LocalMin.X || LocalMax.Y <= LocalMin.Y)
		{
			return false;
		}

		OutChunk.ChunkCoordinate = ChunkCoordinate;
		OutChunk.CellBounds = FIntRect(LocalMin, LocalMax);
		OutChunk.Sections.SetNum(3);

		auto AddQuad = [](
			FSection& Section,
			const FVector& A,
			const FVector& B,
			const FVector& C,
			const FVector& D,
			const FVector& Normal,
			const FVector2D& UvExtent)
			{
				const int32 FirstVertex = Section.Vertices.Num();
				Section.Vertices.Append({ A, B, C, D });
				Section.Normals.Append({ Normal, Normal, Normal, Normal });
				Section.UVs.Append({
					FVector2D::ZeroVector,
					FVector2D(UvExtent.X, 0.0f),
					UvExtent,
					FVector2D(0.0f, UvExtent.Y)
				});
				Section.Triangles.Append({
					FirstVertex,
					FirstVertex + 2,
					FirstVertex + 1,
					FirstVertex,
					FirstVertex + 3,
					FirstVertex + 2
				});
			};

		const int32 CellCountX = LocalMax.X - LocalMin.X;
		const int32 CellCountY = LocalMax.Y - LocalMin.Y;
		const int64 FirstSampleWorldCellX = Terrain.bInfinite
			? ChunkWorldMinX
			: static_cast<int64>(FirstWorldCell.X) + LocalMin.X;
		const int64 FirstSampleWorldCellY = Terrain.bInfinite
			? ChunkWorldMinY
			: static_cast<int64>(FirstWorldCell.Y) + LocalMin.Y;
		constexpr int32 SampleHalo = 3;
		const int32 SampleStride = CellCountX + SampleHalo * 2;
		TArray<float> SampledHeights;
		TArray<uint8> SampledBands;
		SampledHeights.SetNumUninitialized(
			SampleStride * (CellCountY + SampleHalo * 2));
		SampledBands.SetNumUninitialized(SampledHeights.Num());
		const auto ToSampleIndex = [SampleStride](
			const int32 X,
			const int32 Y)
			{
				return (Y + SampleHalo) * SampleStride + X + SampleHalo;
			};
		for (int32 SampleY = -SampleHalo;
			SampleY < CellCountY + SampleHalo;
			++SampleY)
		{
			for (int32 SampleX = -SampleHalo;
				SampleX < CellCountX + SampleHalo;
				++SampleX)
			{
				float Height = Terrain.BottomZ;
				uint8 Band = 0;
				Terrain.TrySampleWorldCell(
					FirstSampleWorldCellX + SampleX,
					FirstSampleWorldCellY + SampleY,
					Height,
					Band);
				const int32 SampleIndex = ToSampleIndex(SampleX, SampleY);
				SampledHeights[SampleIndex] = Height;
				SampledBands[SampleIndex] = Band;
			}
		}

		// Render the exact quantized heightfield, but let physics see a shared
		// piecewise-linear surface without vertical step faces. Each two-cell
		// collision patch takes the upper envelope of every rendered cell touched
		// by either neighboring patch. This keeps the collider smooth without ever
		// placing it below visible terrain, where a grounded capsule would appear
		// buried. World-cell sampling also keeps independently streamed seams
		// identical without coordinating chunk lifetimes.
		FSection& CollisionSurface = OutChunk.CollisionSurface;
		constexpr int32 CollisionCellStride = 2;
		constexpr int32 CollisionSampleMin = -1;
		const int32 CollisionCellCountX =
			FMath::DivideAndRoundUp(CellCountX + 2, CollisionCellStride);
		const int32 CollisionCellCountY =
			FMath::DivideAndRoundUp(CellCountY + 2, CollisionCellStride);
		const int32 CollisionStride = CollisionCellCountX + 1;
		const auto ToCollisionIndex = [CollisionStride](
			const int32 X,
			const int32 Y)
			{
				return Y * CollisionStride + X;
			};
		CollisionSurface.Vertices.Reserve(
			CollisionStride * (CollisionCellCountY + 1));
		CollisionSurface.Normals.SetNumUninitialized(
			CollisionStride * (CollisionCellCountY + 1));
		CollisionSurface.UVs.Reserve(
			CollisionStride * (CollisionCellCountY + 1));
		const double FirstCollisionCenterX = Terrain.FirstCellCenter.X
			+ static_cast<double>(FirstSampleWorldCellX - FirstWorldCell.X)
				* Terrain.CellSize;
		const double FirstCollisionCenterY = Terrain.FirstCellCenter.Y
			+ static_cast<double>(FirstSampleWorldCellY - FirstWorldCell.Y)
				* Terrain.CellSize;
		const double CollisionOriginX =
			FirstCollisionCenterX - Terrain.CellSize * 0.5;
		const double CollisionOriginY =
			FirstCollisionCenterY - Terrain.CellSize * 0.5;
		for (int32 GridY = 0; GridY <= CollisionCellCountY; ++GridY)
		{
			const int32 SampleY = FMath::Min(
				CollisionSampleMin + GridY * CollisionCellStride,
				CellCountY + 1);
			for (int32 GridX = 0; GridX <= CollisionCellCountX; ++GridX)
			{
				const int32 SampleX = FMath::Min(
					CollisionSampleMin + GridX * CollisionCellStride,
					CellCountX + 1);
				double Height = -TNumericLimits<double>::Max();
				for (int32 OffsetY = -CollisionCellStride;
					OffsetY < CollisionCellStride;
					++OffsetY)
				{
					for (int32 OffsetX = -CollisionCellStride;
						OffsetX < CollisionCellStride;
						++OffsetX)
					{
						Height = FMath::Max(
							Height,
							static_cast<double>(SampledHeights[
								ToSampleIndex(
									SampleX + OffsetX,
									SampleY + OffsetY)]));
					}
				}
				const double WorldX =
					CollisionOriginX + SampleX * Terrain.CellSize;
				const double WorldY =
					CollisionOriginY + SampleY * Terrain.CellSize;
				if (!FMath::IsFinite(Height)
					|| !FMath::IsFinite(WorldX)
					|| !FMath::IsFinite(WorldY))
				{
					OutChunk = FChunk();
					return false;
				}
				CollisionSurface.Vertices.Add(
					FVector(WorldX, WorldY, Height));
				CollisionSurface.UVs.Add(FVector2D(SampleX, SampleY));
			}
		}
		for (int32 Y = 0; Y <= CollisionCellCountY; ++Y)
		{
			for (int32 X = 0; X <= CollisionCellCountX; ++X)
			{
				const int32 LeftX = FMath::Max(X - 1, 0);
				const int32 RightX = FMath::Min(
					X + 1,
					CollisionCellCountX);
				const int32 SouthY = FMath::Max(Y - 1, 0);
				const int32 NorthY = FMath::Min(
					Y + 1,
					CollisionCellCountY);
				const double SlopeX = (
					CollisionSurface.Vertices[
						ToCollisionIndex(RightX, Y)].Z
					- CollisionSurface.Vertices[
						ToCollisionIndex(LeftX, Y)].Z)
					/ (CollisionSurface.Vertices[
						ToCollisionIndex(RightX, Y)].X
						- CollisionSurface.Vertices[
							ToCollisionIndex(LeftX, Y)].X);
				const double SlopeY = (
					CollisionSurface.Vertices[
						ToCollisionIndex(X, NorthY)].Z
					- CollisionSurface.Vertices[
						ToCollisionIndex(X, SouthY)].Z)
					/ (CollisionSurface.Vertices[
						ToCollisionIndex(X, NorthY)].Y
						- CollisionSurface.Vertices[
							ToCollisionIndex(X, SouthY)].Y);
				CollisionSurface.Normals[ToCollisionIndex(X, Y)] =
					FVector(-SlopeX, -SlopeY, 1.0).GetSafeNormal();
			}
		}
		CollisionSurface.Triangles.Reserve(
			CollisionCellCountX * CollisionCellCountY * 6);
		for (int32 Y = 0; Y < CollisionCellCountY; ++Y)
		{
			for (int32 X = 0; X < CollisionCellCountX; ++X)
			{
				const int32 A = ToCollisionIndex(X, Y);
				const int32 B = ToCollisionIndex(X + 1, Y);
				const int32 C = ToCollisionIndex(X + 1, Y + 1);
				const int32 D = ToCollisionIndex(X, Y + 1);
				CollisionSurface.Triangles.Append({A, C, B, A, D, C});
			}
		}

		const float HalfCell = Terrain.CellSize * 0.5f;
		// Greedily merge adjacent coplanar cells in the same material band. This
		// preserves the exact rendered heightfield while avoiding four render
		// vertices for every pixel on broad flat steps.
		TBitArray<> MergedTopCells(false, CellCountX * CellCountY);
		const auto ToCellIndex = [CellCountX](const int32 X, const int32 Y)
			{
				return Y * CellCountX + X;
			};
		for (int32 Y = 0; Y < CellCountY; ++Y)
		{
			for (int32 X = 0; X < CellCountX; ++X)
			{
				if (MergedTopCells[ToCellIndex(X, Y)])
				{
					continue;
				}
				const int32 SampleIndex = ToSampleIndex(X, Y);
				const float Height = SampledHeights[SampleIndex];
				const uint8 Band = SampledBands[SampleIndex];
				if (!OutChunk.Sections.IsValidIndex(Band)
					|| !FMath::IsFinite(Height))
				{
					OutChunk = FChunk();
					return false;
				}

				int32 RectangleWidth = 1;
				while (X + RectangleWidth < CellCountX)
				{
					const int32 CandidateX = X + RectangleWidth;
					const int32 CandidateCell = ToCellIndex(CandidateX, Y);
					const int32 CandidateSample = ToSampleIndex(CandidateX, Y);
					if (MergedTopCells[CandidateCell]
						|| SampledBands[CandidateSample] != Band
						|| SampledHeights[CandidateSample] != Height)
					{
						break;
					}
					++RectangleWidth;
				}

				int32 RectangleHeight = 1;
				bool bCanExtend = true;
				while (Y + RectangleHeight < CellCountY && bCanExtend)
				{
					const int32 CandidateY = Y + RectangleHeight;
					for (int32 OffsetX = 0;
						OffsetX < RectangleWidth;
						++OffsetX)
					{
						const int32 CandidateX = X + OffsetX;
						const int32 CandidateCell =
							ToCellIndex(CandidateX, CandidateY);
						const int32 CandidateSample =
							ToSampleIndex(CandidateX, CandidateY);
						if (MergedTopCells[CandidateCell]
							|| SampledBands[CandidateSample] != Band
							|| SampledHeights[CandidateSample] != Height)
						{
							bCanExtend = false;
							break;
						}
					}
					if (bCanExtend)
					{
						++RectangleHeight;
					}
				}

				for (int32 OffsetY = 0;
					OffsetY < RectangleHeight;
					++OffsetY)
				{
					for (int32 OffsetX = 0;
						OffsetX < RectangleWidth;
						++OffsetX)
					{
						MergedTopCells[ToCellIndex(X + OffsetX, Y + OffsetY)] = true;
					}
				}

				const int64 WorldCellX = FirstSampleWorldCellX + X;
				const int64 WorldCellY = FirstSampleWorldCellY + Y;
				const double CenterX = Terrain.FirstCellCenter.X
					+ static_cast<double>(WorldCellX - FirstWorldCell.X)
						* Terrain.CellSize;
				const double CenterY = Terrain.FirstCellCenter.Y
					+ static_cast<double>(WorldCellY - FirstWorldCell.Y)
						* Terrain.CellSize;
				const double X0 = CenterX - HalfCell;
				const double Y0 = CenterY - HalfCell;
				const double X1 = X0 + RectangleWidth * Terrain.CellSize;
				const double Y1 = Y0 + RectangleHeight * Terrain.CellSize;
				AddQuad(
					OutChunk.Sections[Band],
					FVector(X0, Y0, Height),
					FVector(X1, Y0, Height),
					FVector(X1, Y1, Height),
					FVector(X0, Y1, Height),
					FVector::UpVector,
					FVector2D(RectangleWidth, RectangleHeight));
			}
		}

		for (int32 Y = 0; Y < CellCountY; ++Y)
		{
			for (int32 X = 0; X < CellCountX; ++X)
			{
				const int32 SampleIndex = ToSampleIndex(X, Y);
				const uint8 Band = SampledBands[SampleIndex];
				if (!OutChunk.Sections.IsValidIndex(Band))
				{
					OutChunk = FChunk();
					return false;
				}
				FSection& Section = OutChunk.Sections[Band];
				const int64 WorldCellX = FirstSampleWorldCellX + X;
				const int64 WorldCellY = FirstSampleWorldCellY + Y;
				const double CenterX = Terrain.FirstCellCenter.X
					+ static_cast<double>(WorldCellX - FirstWorldCell.X)
						* Terrain.CellSize;
				const double CenterY = Terrain.FirstCellCenter.Y
					+ static_cast<double>(WorldCellY - FirstWorldCell.Y)
						* Terrain.CellSize;
				const double X0 = CenterX - HalfCell;
				const double X1 = CenterX + HalfCell;
				const double Y0 = CenterY - HalfCell;
				const double Y1 = CenterY + HalfCell;
				const float Height = SampledHeights[SampleIndex];
				if (!FMath::IsFinite(Height))
				{
					OutChunk = FChunk();
					return false;
				}

				const float EastHeight =
					SampledHeights[ToSampleIndex(X + 1, Y)];
				if (EastHeight < Height)
				{
					AddQuad(
						Section,
						FVector(X1, Y0, EastHeight),
						FVector(X1, Y1, EastHeight),
						FVector(X1, Y1, Height),
						FVector(X1, Y0, Height),
						FVector::ForwardVector,
						FVector2D(1.0f, (Height - EastHeight) / Terrain.CellSize));
				}
				const float WestHeight =
					SampledHeights[ToSampleIndex(X - 1, Y)];
				if (WestHeight < Height)
				{
					AddQuad(
						Section,
						FVector(X0, Y1, WestHeight),
						FVector(X0, Y0, WestHeight),
						FVector(X0, Y0, Height),
						FVector(X0, Y1, Height),
						-FVector::ForwardVector,
						FVector2D(1.0f, (Height - WestHeight) / Terrain.CellSize));
				}
				const float NorthHeight =
					SampledHeights[ToSampleIndex(X, Y + 1)];
				if (NorthHeight < Height)
				{
					AddQuad(
						Section,
						FVector(X1, Y1, NorthHeight),
						FVector(X0, Y1, NorthHeight),
						FVector(X0, Y1, Height),
						FVector(X1, Y1, Height),
						FVector::RightVector,
						FVector2D(1.0f, (Height - NorthHeight) / Terrain.CellSize));
				}
				const float SouthHeight =
					SampledHeights[ToSampleIndex(X, Y - 1)];
				if (SouthHeight < Height)
				{
					AddQuad(
						Section,
						FVector(X0, Y0, SouthHeight),
						FVector(X1, Y0, SouthHeight),
						FVector(X1, Y0, Height),
						FVector(X0, Y0, Height),
						-FVector::RightVector,
						FVector2D(1.0f, (Height - SouthHeight) / Terrain.CellSize));
				}
			}
		}
		return OutChunk.IsValid();
	}
}
