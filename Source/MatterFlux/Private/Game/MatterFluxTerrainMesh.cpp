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
		const FIntPoint LocalMin(
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
		const FIntPoint LocalMax(
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
		if (LocalMin.X >= Terrain.Width
			|| LocalMin.Y >= Terrain.Height
			|| LocalMax.X <= LocalMin.X
			|| LocalMax.Y <= LocalMin.Y)
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

		const float HalfCell = Terrain.CellSize * 0.5f;
		auto NeighborHeight = [&Terrain](
			const int32 X,
			const int32 Y)
			{
				return X >= 0
					&& X < Terrain.Width
					&& Y >= 0
					&& Y < Terrain.Height
					? Terrain.HeightAt(X, Y)
					: Terrain.BottomZ;
			};

		for (int32 Y = LocalMin.Y; Y < LocalMax.Y; ++Y)
		{
			for (int32 X = LocalMin.X; X < LocalMax.X; ++X)
			{
				const int32 CellIndex = Terrain.ToIndex(X, Y);
				const uint8 Band = Terrain.ColorBands[CellIndex];
				if (!OutChunk.Sections.IsValidIndex(Band))
				{
					OutChunk = FChunk();
					return false;
				}
				FSection& Section = OutChunk.Sections[Band];
				const float CenterX =
					Terrain.FirstCellCenter.X + X * Terrain.CellSize;
				const float CenterY =
					Terrain.FirstCellCenter.Y + Y * Terrain.CellSize;
				const float X0 = CenterX - HalfCell;
				const float X1 = CenterX + HalfCell;
				const float Y0 = CenterY - HalfCell;
				const float Y1 = CenterY + HalfCell;
				const float Height = Terrain.Heights[CellIndex];
				if (!FMath::IsFinite(Height))
				{
					OutChunk = FChunk();
					return false;
				}

				AddQuad(
					Section,
					FVector(X0, Y0, Height),
					FVector(X1, Y0, Height),
					FVector(X1, Y1, Height),
					FVector(X0, Y1, Height),
					FVector::UpVector,
					FVector2D(1.0f, 1.0f));

				const float EastHeight = NeighborHeight(X + 1, Y);
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
				const float WestHeight = NeighborHeight(X - 1, Y);
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
				const float NorthHeight = NeighborHeight(X, Y + 1);
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
				const float SouthHeight = NeighborHeight(X, Y - 1);
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
