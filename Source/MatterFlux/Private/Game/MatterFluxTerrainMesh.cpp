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
		const int32 SampleStride = CellCountX + 2;
		TArray<float> SampledHeights;
		TArray<uint8> SampledBands;
		SampledHeights.SetNumUninitialized(
			SampleStride * (CellCountY + 2));
		SampledBands.SetNumUninitialized(SampledHeights.Num());
		const auto ToSampleIndex = [SampleStride](
			const int32 X,
			const int32 Y)
			{
				return (Y + 1) * SampleStride + X + 1;
			};
		for (int32 SampleY = -1; SampleY <= CellCountY; ++SampleY)
		{
			for (int32 SampleX = -1; SampleX <= CellCountX; ++SampleX)
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

		const float HalfCell = Terrain.CellSize * 0.5f;
		// Greedily merge adjacent coplanar cells in the same material band. This
		// preserves the exact per-cell heightfield while avoiding four render and
		// collision vertices for every pixel on broad flat steps.
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
