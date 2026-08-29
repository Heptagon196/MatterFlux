#include "Game/MatterFluxTerrainMesh.h"

namespace MatterFlux::TerrainMesh
{
	namespace
	{
		void AddQuad(
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
		}

		void AppendCollisionSection(
			const FSection& Source,
			FSection& Destination)
		{
			const int32 FirstVertex = Destination.Vertices.Num();
			Destination.Vertices.Append(Source.Vertices);
			Destination.Normals.Append(Source.Normals);
			Destination.UVs.Append(Source.UVs);
			Destination.Triangles.Reserve(
				Destination.Triangles.Num() + Source.Triangles.Num());
			for (const int32 Index : Source.Triangles)
			{
				Destination.Triangles.Add(FirstVertex + Index);
			}
		}

		bool FindMaterialAtN(
			const TConstArrayView<FMaterialSpan> Spans,
			const int32 N,
			uint16& OutMaterialIndex)
		{
			for (const FMaterialSpan& Span : Spans)
			{
				if (N < Span.BeginN)
				{
					break;
				}
				if (N < Span.EndNExclusive)
				{
					OutMaterialIndex = Span.MaterialIndex;
					return true;
				}
			}
			return false;
		}

		struct FGreedyFaceGroupKey
		{
			uint32 Slot = 0;
			EMaterialSurfaceFace Face = EMaterialSurfaceFace::PositiveN;
			int32 Plane = 0;

			bool operator==(const FGreedyFaceGroupKey& Other) const = default;
		};

		uint32 GetTypeHash(const FGreedyFaceGroupKey& Key)
		{
			return HashCombineFast(
				HashCombineFast(::GetTypeHash(Key.Slot), ::GetTypeHash(Key.Plane)),
				::GetTypeHash(static_cast<uint8>(Key.Face)));
		}
	}

	bool FVolumeSnapshot::IsValid(FString* OutError) const
	{
		auto Fail = [OutError](const TCHAR* Message)
		{
			if (OutError)
			{
				*OutError = Message;
			}
			return false;
		};
		if (SoilMaterialIndex == 0 || SurfaceMaterialIndex == 0)
		{
			return Fail(TEXT("terrain volume snapshot lacks baseline materials"));
		}
		for (const TPair<FIntPoint, TArray<FMaterialSpan>>& Pair
			: ColumnOverrides)
		{
			TArray<FMaterialSpan> Normalized = Pair.Value;
			FString Error;
			if (!FMaterialSpanAlgorithms::Normalize(Normalized, Error)
				|| Normalized != Pair.Value)
			{
				return Fail(TEXT("terrain volume snapshot contains a non-canonical column"));
			}
		}
		for (const TPair<uint16, FLinearColor>& Pair : MaterialColors)
		{
			if (Pair.Key == 0
				|| !FMath::IsFinite(Pair.Value.R)
				|| !FMath::IsFinite(Pair.Value.G)
				|| !FMath::IsFinite(Pair.Value.B)
				|| !FMath::IsFinite(Pair.Value.A))
			{
				return Fail(TEXT("terrain volume snapshot contains an invalid material color"));
			}
		}
		if (OutError)
		{
			OutError->Reset();
		}
		return true;
	}

	bool FVolumeSnapshot::HasVolumeFactsNearChunk(
		const FIntPoint ChunkCoordinate,
		const int32 ChunkSize) const
	{
		if (ChunkSize <= 0 || ColumnOverrides.IsEmpty())
		{
			return false;
		}
		const int64 MinX = static_cast<int64>(ChunkCoordinate.X) * ChunkSize - 1;
		const int64 MinY = static_cast<int64>(ChunkCoordinate.Y) * ChunkSize - 1;
		const int64 MaxX = MinX + ChunkSize + 1;
		const int64 MaxY = MinY + ChunkSize + 1;
		for (const TPair<FIntPoint, TArray<FMaterialSpan>>& Pair
			: ColumnOverrides)
		{
			if (Pair.Key.X >= MinX && Pair.Key.X <= MaxX
				&& Pair.Key.Y >= MinY && Pair.Key.Y <= MaxY)
			{
				return true;
			}
		}
		return false;
	}

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
		for (int32 Band = 0; Band < OutChunk.Sections.Num(); ++Band)
		{
			if (Terrain.BandColors.IsValidIndex(Band))
			{
				OutChunk.Sections[Band].Color = Terrain.BandColors[Band];
			}
		}

		auto AddHeightfieldQuad = [](
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
		// piecewise-linear surface without vertical step faces. Collision vertices
		// pass through the canonical height at every terrain-cell center, then join
		// adjacent centers with slopes. Using the highest surrounding cell at each
		// corner made high banks expand into neighboring water cells as invisible
		// shelves. Center samples preserve smooth movement without dilating land.
		// World-cell sampling also keeps independently streamed seams identical
		// without coordinating chunk lifetimes.
		FSection& CollisionSurface = OutChunk.CollisionSurface;
		constexpr int32 CollisionCellStride = 1;
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
				const double Height = static_cast<double>(
					SampledHeights[ToSampleIndex(SampleX, SampleY)]);
				const double WorldX =
					FirstCollisionCenterX + SampleX * Terrain.CellSize;
				const double WorldY =
					FirstCollisionCenterY + SampleY * Terrain.CellSize;
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
		TBitArray<> MergedCollisionCells(
			false,
			CollisionCellCountX * CollisionCellCountY);
		const auto ToCollisionCellIndex = [CollisionCellCountX](
			const int32 X,
			const int32 Y)
			{
				return Y * CollisionCellCountX + X;
			};
		for (int32 Y = 0; Y < CollisionCellCountY; ++Y)
		{
			for (int32 X = 0; X < CollisionCellCountX; ++X)
			{
				if (MergedCollisionCells[ToCollisionCellIndex(X, Y)])
				{
					continue;
				}
				const double Height = CollisionSurface.Vertices[
					ToCollisionIndex(X, Y)].Z;
				const auto IsFlatCellAtHeight = [
					&CollisionSurface,
					&ToCollisionIndex,
					Height](const int32 CellX, const int32 CellY)
					{
						return CollisionSurface.Vertices[
							ToCollisionIndex(CellX, CellY)].Z == Height
							&& CollisionSurface.Vertices[
								ToCollisionIndex(CellX + 1, CellY)].Z == Height
							&& CollisionSurface.Vertices[
								ToCollisionIndex(CellX + 1, CellY + 1)].Z == Height
							&& CollisionSurface.Vertices[
								ToCollisionIndex(CellX, CellY + 1)].Z == Height;
					};
				int32 RectangleWidth = 1;
				int32 RectangleHeight = 1;
				if (IsFlatCellAtHeight(X, Y))
				{
					while (X + RectangleWidth < CollisionCellCountX
						&& !MergedCollisionCells[ToCollisionCellIndex(
							X + RectangleWidth, Y)]
						&& IsFlatCellAtHeight(X + RectangleWidth, Y))
					{
						++RectangleWidth;
					}
					bool bCanExtend = true;
					while (Y + RectangleHeight < CollisionCellCountY
						&& bCanExtend)
					{
						for (int32 OffsetX = 0;
							OffsetX < RectangleWidth;
							++OffsetX)
						{
							const int32 CandidateX = X + OffsetX;
							const int32 CandidateY = Y + RectangleHeight;
							if (MergedCollisionCells[ToCollisionCellIndex(
								CandidateX, CandidateY)]
								|| !IsFlatCellAtHeight(
									CandidateX, CandidateY))
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
				}
				for (int32 OffsetY = 0;
					OffsetY < RectangleHeight;
					++OffsetY)
				{
					for (int32 OffsetX = 0;
						OffsetX < RectangleWidth;
						++OffsetX)
					{
						MergedCollisionCells[ToCollisionCellIndex(
							X + OffsetX, Y + OffsetY)] = true;
					}
				}
				const int32 A = ToCollisionIndex(X, Y);
				const int32 B = ToCollisionIndex(X + RectangleWidth, Y);
				const int32 C = ToCollisionIndex(
					X + RectangleWidth, Y + RectangleHeight);
				const int32 D = ToCollisionIndex(X, Y + RectangleHeight);
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
				AddHeightfieldQuad(
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
					AddHeightfieldQuad(
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
					AddHeightfieldQuad(
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
					AddHeightfieldQuad(
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
					AddHeightfieldQuad(
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

	bool BuildChunk(
		const PlayableLevel::FLevelTerrain& Terrain,
		const FVolumeSnapshot& Volume,
		const FIntPoint ChunkCoordinate,
		const int32 ChunkSize,
		FChunk& OutChunk)
	{
		FString SnapshotError;
		if (!Volume.IsValid(&SnapshotError))
		{
			OutChunk = FChunk();
			return false;
		}
		if (!Volume.HasVolumeFactsNearChunk(ChunkCoordinate, ChunkSize))
		{
			return BuildChunk(Terrain, ChunkCoordinate, ChunkSize, OutChunk);
		}

		// Reuse the established overflow/bounds validation. The heightfield output is
		// discarded as soon as the finite local cell rectangle has been obtained.
		FChunk BoundsChunk;
		if (!BuildChunk(Terrain, ChunkCoordinate, ChunkSize, BoundsChunk))
		{
			OutChunk = FChunk();
			return false;
		}
		OutChunk = FChunk();
		OutChunk.ChunkCoordinate = ChunkCoordinate;
		OutChunk.CellBounds = BoundsChunk.CellBounds;
		OutChunk.bUsesVolumeSurface = true;

		const FIntPoint FirstWorldCell(
			FMath::FloorToInt(Terrain.FirstCellCenter.X / Terrain.CellSize),
			FMath::FloorToInt(Terrain.FirstCellCenter.Y / Terrain.CellSize));
		const int64 ChunkWorldMinX =
			static_cast<int64>(ChunkCoordinate.X) * ChunkSize;
		const int64 ChunkWorldMinY =
			static_cast<int64>(ChunkCoordinate.Y) * ChunkSize;
		const int64 FirstSampleWorldCellX = Terrain.bInfinite
			? ChunkWorldMinX
			: static_cast<int64>(FirstWorldCell.X) + OutChunk.CellBounds.Min.X;
		const int64 FirstSampleWorldCellY = Terrain.bInfinite
			? ChunkWorldMinY
			: static_cast<int64>(FirstWorldCell.Y) + OutChunk.CellBounds.Min.Y;
		const int32 CellCountX = OutChunk.CellBounds.Width();
		const int32 CellCountY = OutChunk.CellBounds.Height();

		struct FResolvedColumn
		{
			TArray<FMaterialSpan> Spans;
			uint8 GeneratedBand = 0;
		};
		TMap<FIntPoint, FResolvedColumn> Columns;
		const auto ResolveColumn = [&Terrain, &Volume](
			const FIntPoint WorldColumn,
			FResolvedColumn& Out) -> bool
		{
			float GeneratedHeight = Terrain.BottomZ;
			if (!Terrain.TrySampleGeneratedWorldCell(
					WorldColumn.X,
					WorldColumn.Y,
					GeneratedHeight,
					Out.GeneratedBand))
			{
				// The finite seed-map boundary is air. Infinite terrain must always
				// resolve through its deterministic generator, so a miss there is invalid.
				return !Terrain.bInfinite;
			}
			if (const TArray<FMaterialSpan>* Override =
				Volume.ColumnOverrides.Find(WorldColumn))
			{
				Out.Spans = *Override;
				return true;
			}
			const int32 EndN = FMath::Max(0, FMath::CeilToInt(
				(GeneratedHeight - Terrain.BottomZ) / Terrain.CellSize));
			if (EndN > 1)
			{
				Out.Spans.Add(FMaterialSpan(
					0, EndN - 1, Volume.SoilMaterialIndex));
			}
			if (EndN > 0)
			{
				Out.Spans.Add(FMaterialSpan(
					EndN - 1, EndN, Volume.SurfaceMaterialIndex));
			}
			return true;
		};
		for (int32 Y = -1; Y <= CellCountY; ++Y)
		{
			for (int32 X = -1; X <= CellCountX; ++X)
			{
				const int64 WorldX64 = FirstSampleWorldCellX + X;
				const int64 WorldY64 = FirstSampleWorldCellY + Y;
				if (WorldX64 < MIN_int32 || WorldX64 > MAX_int32
					|| WorldY64 < MIN_int32 || WorldY64 > MAX_int32)
				{
					OutChunk = FChunk();
					return false;
				}
				const FIntPoint WorldColumn(
					static_cast<int32>(WorldX64),
					static_cast<int32>(WorldY64));
				FResolvedColumn& Column = Columns.Add(WorldColumn);
				if (!ResolveColumn(WorldColumn, Column))
				{
					OutChunk = FChunk();
					return false;
				}
			}
		}

		TMap<uint32, FSection> SectionsBySlot;
		TMap<FGreedyFaceGroupKey, TSet<FIntPoint>> FaceCellsByGroup;
		for (int32 Y = 0; Y < CellCountY; ++Y)
		{
			for (int32 X = 0; X < CellCountX; ++X)
			{
				const FIntPoint WorldColumn(
					static_cast<int32>(FirstSampleWorldCellX + X),
					static_cast<int32>(FirstSampleWorldCellY + Y));
				const FResolvedColumn& Center = Columns.FindChecked(WorldColumn);
				TArray<FMaterialSurfaceKey> Faces;
				FString SurfaceError;
				if (!FMaterialTerrainSurfaceAlgorithms::GatherExposedFaces(
						WorldColumn,
						Center.Spans,
						Columns.FindChecked(WorldColumn + FIntPoint(-1, 0)).Spans,
						Columns.FindChecked(WorldColumn + FIntPoint(1, 0)).Spans,
						Columns.FindChecked(WorldColumn + FIntPoint(0, -1)).Spans,
						Columns.FindChecked(WorldColumn + FIntPoint(0, 1)).Spans,
						Faces,
						SurfaceError))
				{
					OutChunk = FChunk();
					return false;
				}

				for (const FMaterialSurfaceKey& Face : Faces)
				{
					const int32 MaterialN =
						Face.Face == EMaterialSurfaceFace::PositiveN
							? Face.SurfaceN - 1
							: Face.SurfaceN;
					uint16 MaterialIndex = 0;
					if (!FindMaterialAtN(
							Center.Spans, MaterialN, MaterialIndex))
					{
						OutChunk = FChunk();
						return false;
					}
					const uint32 Slot = MaterialIndex == Volume.SurfaceMaterialIndex
						? 0x10000u + Center.GeneratedBand
						: MaterialIndex;
					FSection& Section = SectionsBySlot.FindOrAdd(Slot);
					Section.MaterialIndex = MaterialIndex;
					if (MaterialIndex == Volume.SurfaceMaterialIndex
						&& Terrain.BandColors.IsValidIndex(Center.GeneratedBand))
					{
						Section.Color = Terrain.BandColors[Center.GeneratedBand];
					}
					else if (const FLinearColor* Color =
						Volume.MaterialColors.Find(MaterialIndex))
					{
						Section.Color = *Color;
					}
					FGreedyFaceGroupKey GroupKey;
					GroupKey.Slot = Slot;
					GroupKey.Face = Face.Face;
					FIntPoint FaceCell;
					switch (Face.Face)
					{
					case EMaterialSurfaceFace::PositiveN:
					case EMaterialSurfaceFace::NegativeN:
						GroupKey.Plane = Face.SurfaceN;
						FaceCell = WorldColumn;
						break;
					case EMaterialSurfaceFace::PositiveU:
						GroupKey.Plane = WorldColumn.X + 1;
						FaceCell = FIntPoint(WorldColumn.Y, Face.SurfaceN);
						break;
					case EMaterialSurfaceFace::NegativeU:
						GroupKey.Plane = WorldColumn.X;
						FaceCell = FIntPoint(WorldColumn.Y, Face.SurfaceN);
						break;
					case EMaterialSurfaceFace::PositiveV:
						GroupKey.Plane = WorldColumn.Y + 1;
						FaceCell = FIntPoint(WorldColumn.X, Face.SurfaceN);
						break;
					case EMaterialSurfaceFace::NegativeV:
						GroupKey.Plane = WorldColumn.Y;
						FaceCell = FIntPoint(WorldColumn.X, Face.SurfaceN);
						break;
					}
					FaceCellsByGroup.FindOrAdd(GroupKey).Add(FaceCell);
				}
			}
		}

		TArray<FGreedyFaceGroupKey> OrderedFaceGroups;
		FaceCellsByGroup.GenerateKeyArray(OrderedFaceGroups);
		OrderedFaceGroups.Sort([](
			const FGreedyFaceGroupKey& A,
			const FGreedyFaceGroupKey& B)
		{
			if (A.Slot != B.Slot)
			{
				return A.Slot < B.Slot;
			}
			if (A.Face != B.Face)
			{
				return static_cast<uint8>(A.Face)
					< static_cast<uint8>(B.Face);
			}
			return A.Plane < B.Plane;
		});
		const double GridOriginX = Terrain.FirstCellCenter.X
			- Terrain.CellSize * 0.5
			- static_cast<double>(FirstWorldCell.X) * Terrain.CellSize;
		const double GridOriginY = Terrain.FirstCellCenter.Y
			- Terrain.CellSize * 0.5
			- static_cast<double>(FirstWorldCell.Y) * Terrain.CellSize;
		for (const FGreedyFaceGroupKey& GroupKey : OrderedFaceGroups)
		{
			TSet<FIntPoint> Remaining =
				FaceCellsByGroup.FindChecked(GroupKey);
			FSection& Section = SectionsBySlot.FindChecked(GroupKey.Slot);
			while (!Remaining.IsEmpty())
			{
				FIntPoint Start(MAX_int32, MAX_int32);
				for (const FIntPoint Cell : Remaining)
				{
					if (Cell.Y < Start.Y
						|| (Cell.Y == Start.Y && Cell.X < Start.X))
					{
						Start = Cell;
					}
				}
				int32 Width = 1;
				while (Remaining.Contains(
					Start + FIntPoint(Width, 0)))
				{
					++Width;
				}
				int32 Height = 1;
				for (;; ++Height)
				{
					bool bCompleteRow = true;
					for (int32 Offset = 0; Offset < Width; ++Offset)
					{
						if (!Remaining.Contains(
							Start + FIntPoint(Offset, Height)))
						{
							bCompleteRow = false;
							break;
						}
					}
					if (!bCompleteRow)
					{
						break;
					}
				}
				for (int32 Y = 0; Y < Height; ++Y)
				{
					for (int32 X = 0; X < Width; ++X)
					{
						Remaining.Remove(Start + FIntPoint(X, Y));
					}
				}
				const FVector2D UvExtent(Width, Height);
				switch (GroupKey.Face)
				{
				case EMaterialSurfaceFace::PositiveN:
				case EMaterialSurfaceFace::NegativeN:
				{
					const double X0 = GridOriginX
						+ static_cast<double>(Start.X) * Terrain.CellSize;
					const double X1 = X0 + Width * Terrain.CellSize;
					const double Y0 = GridOriginY
						+ static_cast<double>(Start.Y) * Terrain.CellSize;
					const double Y1 = Y0 + Height * Terrain.CellSize;
					const double Z = Terrain.BottomZ
						+ static_cast<double>(GroupKey.Plane)
							* Terrain.CellSize;
					if (GroupKey.Face == EMaterialSurfaceFace::PositiveN)
					{
						AddQuad(Section,
							FVector(X0, Y0, Z), FVector(X1, Y0, Z),
							FVector(X1, Y1, Z), FVector(X0, Y1, Z),
							FVector::UpVector, UvExtent);
					}
					else
					{
						AddQuad(Section,
							FVector(X0, Y1, Z), FVector(X1, Y1, Z),
							FVector(X1, Y0, Z), FVector(X0, Y0, Z),
							-FVector::UpVector, UvExtent);
					}
					break;
				}
				case EMaterialSurfaceFace::PositiveU:
				case EMaterialSurfaceFace::NegativeU:
				{
					const double X = GridOriginX
						+ static_cast<double>(GroupKey.Plane)
							* Terrain.CellSize;
					const double Y0 = GridOriginY
						+ static_cast<double>(Start.X) * Terrain.CellSize;
					const double Y1 = Y0 + Width * Terrain.CellSize;
					const double Z0 = Terrain.BottomZ
						+ static_cast<double>(Start.Y) * Terrain.CellSize;
					const double Z1 = Z0 + Height * Terrain.CellSize;
					if (GroupKey.Face == EMaterialSurfaceFace::PositiveU)
					{
						AddQuad(Section,
							FVector(X, Y0, Z0), FVector(X, Y1, Z0),
							FVector(X, Y1, Z1), FVector(X, Y0, Z1),
							FVector::ForwardVector, UvExtent);
					}
					else
					{
						AddQuad(Section,
							FVector(X, Y1, Z0), FVector(X, Y0, Z0),
							FVector(X, Y0, Z1), FVector(X, Y1, Z1),
							-FVector::ForwardVector, UvExtent);
					}
					break;
				}
				case EMaterialSurfaceFace::PositiveV:
				case EMaterialSurfaceFace::NegativeV:
				{
					const double X0 = GridOriginX
						+ static_cast<double>(Start.X) * Terrain.CellSize;
					const double X1 = X0 + Width * Terrain.CellSize;
					const double Y = GridOriginY
						+ static_cast<double>(GroupKey.Plane)
							* Terrain.CellSize;
					const double Z0 = Terrain.BottomZ
						+ static_cast<double>(Start.Y) * Terrain.CellSize;
					const double Z1 = Z0 + Height * Terrain.CellSize;
					if (GroupKey.Face == EMaterialSurfaceFace::PositiveV)
					{
						AddQuad(Section,
							FVector(X1, Y, Z0), FVector(X0, Y, Z0),
							FVector(X0, Y, Z1), FVector(X1, Y, Z1),
							FVector::RightVector, UvExtent);
					}
					else
					{
						AddQuad(Section,
							FVector(X0, Y, Z0), FVector(X1, Y, Z0),
							FVector(X1, Y, Z1), FVector(X0, Y, Z1),
							-FVector::RightVector, UvExtent);
					}
					break;
				}
				}
			}
		}

		TArray<uint32> OrderedSlots;
		SectionsBySlot.GenerateKeyArray(OrderedSlots);
		OrderedSlots.Sort();
		for (const uint32 Slot : OrderedSlots)
		{
			FSection Section = MoveTemp(SectionsBySlot.FindChecked(Slot));
			if (!Section.IsValid())
			{
				OutChunk = FChunk();
				return false;
			}
			AppendCollisionSection(Section, OutChunk.CollisionSurface);
			OutChunk.Sections.Add(MoveTemp(Section));
		}
		return OutChunk.IsValid();
	}

	bool SweepVolumeSurface(
		const PlayableLevel::FLevelTerrain& Terrain,
		const FVolumeSnapshot& Volume,
		const FVector& LocalStart,
		const FVector& LocalEnd,
		const float Radius,
		FSurfaceHit& OutHit)
	{
		OutHit = FSurfaceHit();
		if (!Terrain.IsValid()
			|| !Volume.IsValid()
			|| LocalStart.ContainsNaN()
			|| LocalEnd.ContainsNaN()
			|| !FMath::IsFinite(Radius)
			|| Radius < 0.0f
			|| Volume.ColumnOverrides.IsEmpty())
		{
			return false;
		}
		const FIntPoint FirstWorldCell(
			FMath::FloorToInt(Terrain.FirstCellCenter.X / Terrain.CellSize),
			FMath::FloorToInt(Terrain.FirstCellCenter.Y / Terrain.CellSize));
		const double GridOriginX = Terrain.FirstCellCenter.X
			- Terrain.CellSize * 0.5
			- static_cast<double>(FirstWorldCell.X) * Terrain.CellSize;
		const double GridOriginY = Terrain.FirstCellCenter.Y
			- Terrain.CellSize * 0.5
			- static_cast<double>(FirstWorldCell.Y) * Terrain.CellSize;
		const auto ToColumnX = [&Terrain, GridOriginX](const double X)
		{
			return FMath::FloorToInt((X - GridOriginX) / Terrain.CellSize);
		};
		const auto ToColumnY = [&Terrain, GridOriginY](const double Y)
		{
			return FMath::FloorToInt((Y - GridOriginY) / Terrain.CellSize);
		};
		const int32 MinX = ToColumnX(
			FMath::Min(LocalStart.X, LocalEnd.X) - Radius) - 1;
		const int32 MaxX = ToColumnX(
			FMath::Max(LocalStart.X, LocalEnd.X) + Radius) + 1;
		const int32 MinY = ToColumnY(
			FMath::Min(LocalStart.Y, LocalEnd.Y) - Radius) - 1;
		const int32 MaxY = ToColumnY(
			FMath::Max(LocalStart.Y, LocalEnd.Y) + Radius) + 1;
		const int64 ColumnCount =
			(static_cast<int64>(MaxX) - MinX + 1)
			* (static_cast<int64>(MaxY) - MinY + 1);
		if (ColumnCount <= 0 || ColumnCount > 4096)
		{
			return false;
		}
		bool bTouchesSparseRegion = false;
		for (const TPair<FIntPoint, TArray<FMaterialSpan>>& Pair
			: Volume.ColumnOverrides)
		{
			if (Pair.Key.X >= MinX - 1 && Pair.Key.X <= MaxX + 1
				&& Pair.Key.Y >= MinY - 1 && Pair.Key.Y <= MaxY + 1)
			{
				bTouchesSparseRegion = true;
				break;
			}
		}
		if (!bTouchesSparseRegion)
		{
			return false;
		}

		const auto ResolveColumn = [&Terrain, &Volume](
			const FIntPoint WorldColumn,
			TArray<FMaterialSpan>& OutSpans) -> bool
		{
			if (const TArray<FMaterialSpan>* Override =
				Volume.ColumnOverrides.Find(WorldColumn))
			{
				OutSpans = *Override;
				return true;
			}
			float Height = Terrain.BottomZ;
			uint8 Band = 0;
			if (!Terrain.TrySampleGeneratedWorldCell(
					WorldColumn.X, WorldColumn.Y, Height, Band))
			{
				OutSpans.Reset();
				return !Terrain.bInfinite;
			}
			const int32 EndN = FMath::Max(0, FMath::CeilToInt(
				(Height - Terrain.BottomZ) / Terrain.CellSize));
			if (EndN > 1)
			{
				OutSpans.Add(FMaterialSpan(
					0, EndN - 1, Volume.SoilMaterialIndex));
			}
			if (EndN > 0)
			{
				OutSpans.Add(FMaterialSpan(
					EndN - 1, EndN, Volume.SurfaceMaterialIndex));
			}
			return true;
		};
		TMap<FIntPoint, TArray<FMaterialSpan>> Columns;
		for (int32 Y = MinY - 1; Y <= MaxY + 1; ++Y)
		{
			for (int32 X = MinX - 1; X <= MaxX + 1; ++X)
			{
				TArray<FMaterialSpan>& Spans = Columns.Add(FIntPoint(X, Y));
				if (!ResolveColumn(FIntPoint(X, Y), Spans))
				{
					return false;
				}
			}
		}

		const FVector Delta = LocalEnd - LocalStart;
		double BestTime = TNumericLimits<double>::Max();
		bool bFound = false;
		for (int32 Y = MinY; Y <= MaxY; ++Y)
		{
			for (int32 X = MinX; X <= MaxX; ++X)
			{
				const FIntPoint Column(X, Y);
				const TArray<FMaterialSpan>& Center = Columns.FindChecked(Column);
				TArray<FMaterialSurfaceKey> Faces;
				FString Error;
				if (!FMaterialTerrainSurfaceAlgorithms::GatherExposedFaces(
						Column,
						Center,
						Columns.FindChecked(Column + FIntPoint(-1, 0)),
						Columns.FindChecked(Column + FIntPoint(1, 0)),
						Columns.FindChecked(Column + FIntPoint(0, -1)),
						Columns.FindChecked(Column + FIntPoint(0, 1)),
						Faces,
						Error))
				{
					return false;
				}
				const double X0 = GridOriginX
					+ static_cast<double>(X) * Terrain.CellSize;
				const double X1 = X0 + Terrain.CellSize;
				const double Y0 = GridOriginY
					+ static_cast<double>(Y) * Terrain.CellSize;
				const double Y1 = Y0 + Terrain.CellSize;
				for (const FMaterialSurfaceKey& Face : Faces)
				{
					FVector Normal = FVector::UpVector;
					FVector PlanePoint = FVector::ZeroVector;
					const double Z0 = Terrain.BottomZ
						+ static_cast<double>(Face.SurfaceN) * Terrain.CellSize;
					const double Z1 = Z0 + Terrain.CellSize;
					switch (Face.Face)
					{
					case EMaterialSurfaceFace::NegativeU:
						Normal = -FVector::ForwardVector;
						PlanePoint = FVector(X0, Y0, Z0);
						break;
					case EMaterialSurfaceFace::PositiveU:
						Normal = FVector::ForwardVector;
						PlanePoint = FVector(X1, Y0, Z0);
						break;
					case EMaterialSurfaceFace::NegativeV:
						Normal = -FVector::RightVector;
						PlanePoint = FVector(X0, Y0, Z0);
						break;
					case EMaterialSurfaceFace::PositiveV:
						Normal = FVector::RightVector;
						PlanePoint = FVector(X0, Y1, Z0);
						break;
					case EMaterialSurfaceFace::NegativeN:
						Normal = -FVector::UpVector;
						PlanePoint = FVector(X0, Y0, Z0);
						break;
					case EMaterialSurfaceFace::PositiveN:
						Normal = FVector::UpVector;
						PlanePoint = FVector(X0, Y0, Z0);
						break;
					}
					const double StartDistance = FVector::DotProduct(
						LocalStart - PlanePoint, Normal) - Radius;
					const double EndDistance = FVector::DotProduct(
						LocalEnd - PlanePoint, Normal) - Radius;
					if (StartDistance < -KINDA_SMALL_NUMBER
						|| EndDistance > KINDA_SMALL_NUMBER
						|| StartDistance - EndDistance <= SMALL_NUMBER)
					{
						continue;
					}
					const double Time = FMath::Clamp(
						StartDistance / (StartDistance - EndDistance), 0.0, 1.0);
					const FVector Candidate = LocalStart + Delta * Time;
					bool bInsideFace = false;
					switch (Face.Face)
					{
					case EMaterialSurfaceFace::NegativeU:
					case EMaterialSurfaceFace::PositiveU:
						bInsideFace = Candidate.Y >= Y0 - Radius
							&& Candidate.Y <= Y1 + Radius
							&& Candidate.Z >= Z0 - Radius
							&& Candidate.Z <= Z1 + Radius;
						break;
					case EMaterialSurfaceFace::NegativeV:
					case EMaterialSurfaceFace::PositiveV:
						bInsideFace = Candidate.X >= X0 - Radius
							&& Candidate.X <= X1 + Radius
							&& Candidate.Z >= Z0 - Radius
							&& Candidate.Z <= Z1 + Radius;
						break;
					case EMaterialSurfaceFace::NegativeN:
					case EMaterialSurfaceFace::PositiveN:
						bInsideFace = Candidate.X >= X0 - Radius
							&& Candidate.X <= X1 + Radius
							&& Candidate.Y >= Y0 - Radius
							&& Candidate.Y <= Y1 + Radius;
						break;
					}
					if (!bInsideFace || Time > BestTime + UE_DOUBLE_SMALL_NUMBER)
					{
						continue;
					}
					const int32 MaterialN =
						Face.Face == EMaterialSurfaceFace::PositiveN
							? Face.SurfaceN - 1
							: Face.SurfaceN;
					uint16 MaterialIndex = 0;
					if (!FindMaterialAtN(Center, MaterialN, MaterialIndex))
					{
						continue;
					}
					const bool bEarlier = Time < BestTime - UE_DOUBLE_SMALL_NUMBER;
					const bool bTieBreaksBefore = !bEarlier && bFound
						&& (Face.SurfaceN < OutHit.Surface.SurfaceN
							|| (Face.SurfaceN == OutHit.Surface.SurfaceN
								&& static_cast<uint8>(Face.Face)
									< static_cast<uint8>(OutHit.Surface.Face)));
					if (bEarlier || !bFound || bTieBreaksBefore)
					{
						bFound = true;
						BestTime = Time;
						OutHit.Surface = Face;
						OutHit.MaterialIndex = MaterialIndex;
						OutHit.LocalLocation = Candidate;
						OutHit.LocalNormal = Normal;
						OutHit.Time = Time;
					}
				}
			}
		}
		return bFound;
	}
}
