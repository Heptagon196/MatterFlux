#include "Material/MatterFluxMaterialWorld.h"

namespace MatterFlux::Material
{
	namespace
	{
		constexpr uint32 ActiveStateMagic = 0x3153464du;
		constexpr uint16 LegacyActiveStateVersion = 1;
		constexpr uint16 ActiveStateVersion = 2;
		constexpr int32 MaximumActiveStateBytes = 1024 * 1024;

		void AppendUint16(TArray<uint8>& Bytes, const uint16 Value)
		{
			Bytes.Add(static_cast<uint8>(Value));
			Bytes.Add(static_cast<uint8>(Value >> 8u));
		}

		void AppendUint32(TArray<uint8>& Bytes, const uint32 Value)
		{
			Bytes.Add(static_cast<uint8>(Value));
			Bytes.Add(static_cast<uint8>(Value >> 8u));
			Bytes.Add(static_cast<uint8>(Value >> 16u));
			Bytes.Add(static_cast<uint8>(Value >> 24u));
		}

		void AppendInt16(TArray<uint8>& Bytes, const int16 Value)
		{
			AppendUint16(Bytes, static_cast<uint16>(Value));
		}

		void AppendInt32(TArray<uint8>& Bytes, const int32 Value)
		{
			AppendUint32(Bytes, static_cast<uint32>(Value));
		}

		bool ReadUint16(
			const TArray<uint8>& Bytes,
			int32& Offset,
			uint16& OutValue)
		{
			if (Offset < 0 || Offset > Bytes.Num() - 2)
			{
				return false;
			}
			OutValue =
				static_cast<uint16>(Bytes[Offset])
				| static_cast<uint16>(Bytes[Offset + 1]) << 8u;
			Offset += 2;
			return true;
		}

		bool ReadUint32(
			const TArray<uint8>& Bytes,
			int32& Offset,
			uint32& OutValue)
		{
			if (Offset < 0 || Offset > Bytes.Num() - 4)
			{
				return false;
			}
			OutValue =
				static_cast<uint32>(Bytes[Offset])
				| static_cast<uint32>(Bytes[Offset + 1]) << 8u
				| static_cast<uint32>(Bytes[Offset + 2]) << 16u
				| static_cast<uint32>(Bytes[Offset + 3]) << 24u;
			Offset += 4;
			return true;
		}

		bool ReadInt16(
			const TArray<uint8>& Bytes,
			int32& Offset,
			int16& OutValue)
		{
			uint16 Value = 0;
			if (!ReadUint16(Bytes, Offset, Value))
			{
				return false;
			}
			OutValue = static_cast<int16>(Value);
			return true;
		}

		bool ReadInt32(
			const TArray<uint8>& Bytes,
			int32& Offset,
			int32& OutValue)
		{
			uint32 Value = 0;
			if (!ReadUint32(Bytes, Offset, Value))
			{
				return false;
			}
			OutValue = static_cast<int32>(Value);
			return true;
		}

		int32 FloorDivide(const int32 Value, const int32 Divisor)
		{
			check(Divisor > 0);
			const int64 WideValue = static_cast<int64>(Value);
			const int64 WideDivisor = static_cast<int64>(Divisor);
			const int64 Result = WideValue >= 0
				? WideValue / WideDivisor
				: -(((-WideValue) + WideDivisor - 1) / WideDivisor);
			return static_cast<int32>(Result);
		}

		int32 PositiveModulo(const int32 Value, const int32 Divisor)
		{
			const int32 Result = Value % Divisor;
			return Result >= 0 ? Result : Result + Divisor;
		}

		bool TryOffsetCell(
			const FIntPoint& Cell,
			const FIntPoint& Offset,
			FIntPoint& OutCell)
		{
			const int64 X =
				static_cast<int64>(Cell.X) + Offset.X;
			const int64 Y =
				static_cast<int64>(Cell.Y) + Offset.Y;
			if (X < MIN_int32 || X > MAX_int32
				|| Y < MIN_int32 || Y > MAX_int32)
			{
				return false;
			}
			OutCell = FIntPoint(
				static_cast<int32>(X),
				static_cast<int32>(Y));
			return true;
		}

		uint32 MixBits(uint32 Value)
		{
			Value ^= Value >> 16u;
			Value *= 0x7feb352du;
			Value ^= Value >> 15u;
			Value *= 0x846ca68bu;
			Value ^= Value >> 16u;
			return Value;
		}
	}

	struct FChunkedMaterialWorld::FImpl
	{
		struct FResolvedMaterial
		{
			FName Id = NAME_None;
			EMatterFluxMaterialPhase Phase =
				EMatterFluxMaterialPhase::StaticSolid;
			uint16 Density = 1;
			uint8 Mobility = 255;
			uint8 Dispersion = 128;
		};

		struct FCell
		{
			uint16 MaterialIndex = 0;
			int16 SupportHeight = 0;
			uint32 LastUpdatedTick = 0;
		};

		struct FResolvedReaction
		{
			uint16 InputA = 0;
			uint16 InputB = 0;
			uint16 OutputA = 0;
			uint16 OutputB = 0;
			uint16 ChancePermille = 1000;
		};

		struct FChunk
		{
			explicit FChunk(const int32 CellCount)
			{
				Cells.SetNumZeroed(CellCount);
			}

			TArray<FCell> Cells;
			FIntPoint DirtyMin = FIntPoint(MAX_int32, MAX_int32);
			FIntPoint DirtyMax = FIntPoint(MIN_int32, MIN_int32);
			bool bHasDirtyCells = false;

			void MarkDirty(const FIntPoint& LocalCell)
			{
				if (!bHasDirtyCells)
				{
					DirtyMin = LocalCell;
					DirtyMax = LocalCell;
					bHasDirtyCells = true;
					return;
				}
				DirtyMin.X = FMath::Min(DirtyMin.X, LocalCell.X);
				DirtyMin.Y = FMath::Min(DirtyMin.Y, LocalCell.Y);
				DirtyMax.X = FMath::Max(DirtyMax.X, LocalCell.X);
				DirtyMax.Y = FMath::Max(DirtyMax.Y, LocalCell.Y);
			}
		};

		struct FArchivedRun
		{
			uint16 MaterialIndex = 0;
			int16 SupportHeight = 0;
			uint16 Length = 0;
		};

		struct FArchivedChunk
		{
			TArray<FArchivedRun> Runs;
		};

		FWorldSettings Settings;
		TArray<FResolvedMaterial> Materials;
		TMap<FName, uint16> MaterialIndices;
		TArray<FResolvedReaction> Reactions;
		TMap<FIntPoint, TUniquePtr<FChunk>> Chunks;
		TMap<FIntPoint, FArchivedChunk> ArchivedChunks;
		FIntPoint FocusCell = FIntPoint::ZeroValue;
		TArray<FIntPoint> FocusCells;
		TSet<FIntPoint> ActiveChunks;
		uint32 Seed = 0;
		uint32 Tick = 0;
		bool bInitialized = false;

		FIntPoint ToChunkCoordinate(const FIntPoint& WorldCell) const
		{
			return FIntPoint(
				FloorDivide(WorldCell.X, Settings.ChunkSize),
				FloorDivide(WorldCell.Y, Settings.ChunkSize));
		}

		FIntPoint ToLocalCoordinate(const FIntPoint& WorldCell) const
		{
			return FIntPoint(
				PositiveModulo(WorldCell.X, Settings.ChunkSize),
				PositiveModulo(WorldCell.Y, Settings.ChunkSize));
		}

		int32 ToIndex(const FIntPoint& LocalCell) const
		{
			return LocalCell.Y * Settings.ChunkSize + LocalCell.X;
		}

		bool IsInsideVerticalBounds(const int32 WorldY) const
		{
			return WorldY >= Settings.MinWorldHeightCells
				&& WorldY < Settings.MaxWorldHeightCells;
		}

		bool IsInsideSurfaceBounds(
			const FIntPoint& WorldCell) const
		{
			return WorldCell.X >= Settings.MinSurfaceCell.X
				&& WorldCell.Y >= Settings.MinSurfaceCell.Y
				&& WorldCell.X < Settings.MaxSurfaceCellExclusive.X
				&& WorldCell.Y < Settings.MaxSurfaceCellExclusive.Y;
		}

		bool IsChunkActive(const FIntPoint& ChunkCoordinate) const
		{
			return ActiveChunks.Contains(ChunkCoordinate);
		}

		void BuildFocusState(
			TConstArrayView<FIntPoint> WorldCells,
			TArray<FIntPoint>& OutFocusCells,
			TSet<FIntPoint>& OutActiveChunks) const
		{
			struct FFocusEntry
			{
				FIntPoint Cell = FIntPoint::ZeroValue;
				FIntPoint Chunk = FIntPoint::ZeroValue;
			};
			TArray<FFocusEntry> Entries;
			Entries.Reserve(FMath::Max(WorldCells.Num(), 1));
			if (WorldCells.IsEmpty())
			{
				Entries.Add({ FIntPoint::ZeroValue, FIntPoint::ZeroValue });
			}
			else
			{
				for (const FIntPoint Cell : WorldCells)
				{
					Entries.Add({ Cell, ToChunkCoordinate(Cell) });
				}
			}
			Entries.Sort([](const FFocusEntry& A, const FFocusEntry& B)
			{
				if (A.Chunk.X != B.Chunk.X)
				{
					return A.Chunk.X < B.Chunk.X;
				}
				if (A.Chunk.Y != B.Chunk.Y)
				{
					return A.Chunk.Y < B.Chunk.Y;
				}
				return A.Cell.X != B.Cell.X
					? A.Cell.X < B.Cell.X
					: A.Cell.Y < B.Cell.Y;
			});

			OutFocusCells.Reset();
			TArray<FIntPoint> FocusChunks;
			for (const FFocusEntry& Entry : Entries)
			{
				if (!FocusChunks.IsEmpty()
					&& FocusChunks.Last() == Entry.Chunk)
				{
					continue;
				}
				OutFocusCells.Add(Entry.Cell);
				FocusChunks.Add(Entry.Chunk);
			}

			OutActiveChunks.Reset();
			const int32 Radius = Settings.ActiveChunkRadius;
			for (int32 Distance = 0;
				Distance <= Radius
					&& OutActiveChunks.Num() < Settings.MaxActiveChunks;
				++Distance)
			{
				TArray<FIntPoint> RingOffsets;
				for (int32 OffsetY = -Distance;
					OffsetY <= Distance;
					++OffsetY)
				{
					for (int32 OffsetX = -Distance;
						OffsetX <= Distance;
						++OffsetX)
					{
						if (FMath::Max(
							FMath::Abs(OffsetX),
							FMath::Abs(OffsetY)) == Distance)
						{
							RingOffsets.Add(FIntPoint(OffsetX, OffsetY));
						}
					}
				}
				// Traverse one ring coordinate for every focus before moving to
				// the next coordinate. This deterministic round robin prevents
				// the first focus from consuming the whole active-chunk budget.
				for (const FIntPoint Offset : RingOffsets)
				{
					for (const FIntPoint FocusChunk : FocusChunks)
					{
						FIntPoint Candidate;
						if (TryOffsetCell(
							FocusChunk,
							Offset,
							Candidate))
						{
							OutActiveChunks.Add(Candidate);
						}
						if (OutActiveChunks.Num() >= Settings.MaxActiveChunks)
						{
							break;
						}
					}
					if (OutActiveChunks.Num() >= Settings.MaxActiveChunks)
					{
						break;
					}
				}
			}
		}

		void SetFocusCells(TConstArrayView<FIntPoint> WorldCells)
		{
			BuildFocusState(WorldCells, FocusCells, ActiveChunks);
			FocusCell = FocusCells[0];
		}

		FArchivedChunk EncodeChunk(const FChunk& Chunk) const
		{
			FArchivedChunk Archive;
			if (Chunk.Cells.IsEmpty())
			{
				return Archive;
			}

			uint16 CurrentMaterial = Chunk.Cells[0].MaterialIndex;
			int16 CurrentSupport = Chunk.Cells[0].SupportHeight;
			uint16 CurrentLength = 0;
			for (const FCell& Cell : Chunk.Cells)
			{
				if (Cell.MaterialIndex != CurrentMaterial
					|| Cell.SupportHeight != CurrentSupport
					|| CurrentLength == MAX_uint16)
				{
					Archive.Runs.Add({
						CurrentMaterial,
						CurrentSupport,
						CurrentLength });
					CurrentMaterial = Cell.MaterialIndex;
					CurrentSupport = Cell.SupportHeight;
					CurrentLength = 0;
				}
				++CurrentLength;
			}
			Archive.Runs.Add({
				CurrentMaterial,
				CurrentSupport,
				CurrentLength });
			return Archive;
		}

		bool HasPersistentData(const FChunk& Chunk) const
		{
			return Chunk.Cells.ContainsByPredicate([](const FCell& Cell)
			{
				return Cell.MaterialIndex != 0
					|| Cell.SupportHeight != 0;
			});
		}

		void DecodeChunk(
			const FArchivedChunk& Archive,
			FChunk& OutChunk) const
		{
			int32 CellIndex = 0;
			for (const FArchivedRun& Run : Archive.Runs)
			{
				for (int32 RunIndex = 0;
					RunIndex < Run.Length
						&& CellIndex < OutChunk.Cells.Num();
					++RunIndex)
				{
					OutChunk.Cells[CellIndex].MaterialIndex =
						Run.MaterialIndex;
					OutChunk.Cells[CellIndex].SupportHeight =
						Run.SupportHeight;
					OutChunk.Cells[CellIndex].LastUpdatedTick = 0;
					++CellIndex;
				}
			}
		}

		void ArchiveChunk(const FIntPoint& ChunkCoordinate)
		{
			TUniquePtr<FChunk>* Chunk = Chunks.Find(ChunkCoordinate);
			if (!Chunk || !Chunk->IsValid())
			{
				return;
			}
			if (HasPersistentData(**Chunk))
			{
				ArchivedChunks.Add(
					ChunkCoordinate,
					EncodeChunk(**Chunk));
			}
			else
			{
				ArchivedChunks.Remove(ChunkCoordinate);
			}
			Chunks.Remove(ChunkCoordinate);
		}

		FChunk& FindOrAddChunk(const FIntPoint& ChunkCoordinate)
		{
			TUniquePtr<FChunk>& Chunk = Chunks.FindOrAdd(ChunkCoordinate);
			if (!Chunk)
			{
				Chunk = MakeUnique<FChunk>(
					Settings.ChunkSize * Settings.ChunkSize);
				if (FArchivedChunk* Archive =
					ArchivedChunks.Find(ChunkCoordinate))
				{
					DecodeChunk(*Archive, *Chunk);
					ArchivedChunks.Remove(ChunkCoordinate);
				}
			}
			return *Chunk;
		}

		FCell* FindCell(const FIntPoint& WorldCell)
		{
			TUniquePtr<FChunk>* Chunk =
				Chunks.Find(ToChunkCoordinate(WorldCell));
			if (!Chunk || !Chunk->IsValid())
			{
				return nullptr;
			}
			return &(*Chunk)->Cells[ToIndex(ToLocalCoordinate(WorldCell))];
		}

		const FCell* FindCell(const FIntPoint& WorldCell) const
		{
			const TUniquePtr<FChunk>* Chunk =
				Chunks.Find(ToChunkCoordinate(WorldCell));
			if (!Chunk || !Chunk->IsValid())
			{
				return nullptr;
			}
			return &(*Chunk)->Cells[ToIndex(ToLocalCoordinate(WorldCell))];
		}

		uint16 FindArchivedMaterialIndex(
			const FIntPoint& WorldCell) const
		{
			const FArchivedChunk* Archive =
				ArchivedChunks.Find(ToChunkCoordinate(WorldCell));
			if (!Archive)
			{
				return 0;
			}
			int32 RemainingIndex = ToIndex(ToLocalCoordinate(WorldCell));
			for (const FArchivedRun& Run : Archive->Runs)
			{
				if (RemainingIndex < Run.Length)
				{
					return Run.MaterialIndex;
				}
				RemainingIndex -= Run.Length;
			}
			return 0;
		}

		bool FindSupportHeight(
			const FIntPoint& WorldCell,
			int16& OutSupportHeight) const
		{
			if (const FCell* Cell = FindCell(WorldCell))
			{
				OutSupportHeight = Cell->SupportHeight;
				return true;
			}
			const FArchivedChunk* Archive =
				ArchivedChunks.Find(ToChunkCoordinate(WorldCell));
			if (!Archive)
			{
				return false;
			}
			int32 RemainingIndex = ToIndex(ToLocalCoordinate(WorldCell));
			for (const FArchivedRun& Run : Archive->Runs)
			{
				if (RemainingIndex < Run.Length)
				{
					OutSupportHeight = Run.SupportHeight;
					return true;
				}
				RemainingIndex -= Run.Length;
			}
			return false;
		}

		FCell& FindOrAddCell(const FIntPoint& WorldCell)
		{
			FChunk& Chunk = FindOrAddChunk(ToChunkCoordinate(WorldCell));
			return Chunk.Cells[ToIndex(ToLocalCoordinate(WorldCell))];
		}

		void MarkDirty(const FIntPoint& WorldCell)
		{
			const bool bOutsideSimulation =
				Settings.bUseSurfaceTopology
					? Settings.bCullOutsideSurfaceBounds
						&& !IsInsideSurfaceBounds(WorldCell)
					: Settings.bCullOutsideVerticalBounds
						&& !IsInsideVerticalBounds(WorldCell.Y);
			if (bOutsideSimulation)
			{
				return;
			}
			const FIntPoint ChunkCoordinate = ToChunkCoordinate(WorldCell);
			if (!IsChunkActive(ChunkCoordinate))
			{
				return;
			}
			FindOrAddChunk(ChunkCoordinate).MarkDirty(
				ToLocalCoordinate(WorldCell));
		}

		void MarkDirtyNeighborhood(const FIntPoint& WorldCell)
		{
			for (int32 Y = -1; Y <= 1; ++Y)
			{
				for (int32 X = -1; X <= 1; ++X)
				{
					FIntPoint Neighbor;
					if (TryOffsetCell(
						WorldCell,
						FIntPoint(X, Y),
						Neighbor))
					{
						MarkDirty(Neighbor);
					}
				}
			}
		}

		bool PassesMobilityCheck(
			const FIntPoint& WorldCell,
			const FResolvedMaterial& Material) const
		{
			if (Material.Mobility == 255)
			{
				return true;
			}
			const uint32 Hash = MixBits(
				Seed
				^ static_cast<uint32>(WorldCell.X) * 0x9e3779b9u
				^ static_cast<uint32>(WorldCell.Y) * 0x85ebca6bu
				^ Tick * 0xc2b2ae35u);
			return static_cast<uint8>(Hash & 0xffu) < Material.Mobility;
		}

		bool TryMoveSurfaceCell(
			const FIntPoint& Source,
			const FResolvedMaterial& Material,
			FStepStats& Stats)
		{
			static const FIntPoint Directions[] =
			{
				FIntPoint(1, 0),
				FIntPoint(0, 1),
				FIntPoint(-1, 0),
				FIntPoint(0, -1),
				FIntPoint(1, 1),
				FIntPoint(-1, 1),
				FIntPoint(-1, -1),
				FIntPoint(1, -1)
			};
			const uint32 DirectionHash = MixBits(
				Seed
					^ static_cast<uint32>(Source.X) * 0x9e3779b9u
					^ static_cast<uint32>(Source.Y) * 0x85ebca6bu
					^ Tick * 0xc2b2ae35u);
			const int32 FirstDirection =
				static_cast<int32>(DirectionHash % UE_ARRAY_COUNT(Directions));

			if (Material.Phase == EMatterFluxMaterialPhase::Gas)
			{
				for (int32 Offset = 0;
					Offset < UE_ARRAY_COUNT(Directions);
					++Offset)
				{
					FIntPoint Destination;
					if (!TryOffsetCell(
						Source,
						Directions[
							(FirstDirection + Offset)
								% UE_ARRAY_COUNT(Directions)],
						Destination))
					{
						continue;
					}
					if (Settings.bCullOutsideSurfaceBounds
						&& !IsInsideSurfaceBounds(Destination))
					{
						return TryMoveCell(
							Source,
							Destination,
							Stats,
							true);
					}
					int16 DestinationSupport = 0;
					const FCell* DestinationCell =
						FindCell(Destination);
					if (!FindSupportHeight(
							Destination,
							DestinationSupport)
						|| (DestinationCell
							&& DestinationCell->MaterialIndex != 0))
					{
						continue;
					}
					if (TryMoveCell(
						Source,
						Destination,
						Stats,
						true))
					{
						return true;
					}
				}
				return false;
			}

			int16 SourceSupport = 0;
			if (!FindSupportHeight(Source, SourceSupport))
			{
				return false;
			}

			bool bFoundDestination = false;
			int16 LowestSupport = MAX_int16;
			FIntPoint BestDestination = Source;
			for (int32 Offset = 0;
				Offset < UE_ARRAY_COUNT(Directions);
				++Offset)
			{
				FIntPoint Destination;
				if (!TryOffsetCell(
					Source,
					Directions[
						(FirstDirection + Offset)
							% UE_ARRAY_COUNT(Directions)],
					Destination))
				{
					continue;
				}
				if (Settings.bCullOutsideSurfaceBounds
					&& !IsInsideSurfaceBounds(Destination))
				{
					continue;
				}
				int16 DestinationSupport = 0;
				const FCell* DestinationCell = FindCell(Destination);
				if (!FindSupportHeight(
						Destination,
						DestinationSupport)
					|| (DestinationCell
						&& DestinationCell->MaterialIndex != 0)
					|| DestinationSupport > SourceSupport
					|| (Material.Phase
							== EMatterFluxMaterialPhase::Powder
						&& DestinationSupport >= SourceSupport))
				{
					continue;
				}
				if (!bFoundDestination
					|| DestinationSupport < LowestSupport)
				{
					bFoundDestination = true;
					LowestSupport = DestinationSupport;
					BestDestination = Destination;
				}
			}
			if (!bFoundDestination)
			{
				return false;
			}
			if (LowestSupport == SourceSupport
				&& static_cast<uint8>(
					(DirectionHash >> 8u) & 0xffu)
					>= Material.Dispersion)
			{
				MarkDirty(Source);
				return false;
			}
			return TryMoveCell(
				Source,
				BestDestination,
				Stats);
		}

		bool TryMoveCell(
			const FIntPoint& Source,
			const FIntPoint& Destination,
			FStepStats& Stats,
			const bool bRising = false)
		{
			const bool bOutsideSimulation =
				Settings.bUseSurfaceTopology
					? Settings.bCullOutsideSurfaceBounds
						&& !IsInsideSurfaceBounds(Destination)
					: Settings.bCullOutsideVerticalBounds
						&& !IsInsideVerticalBounds(Destination.Y);
			if (bOutsideSimulation)
			{
				FCell& SourceCell = FindOrAddCell(Source);
				SourceCell.MaterialIndex = 0;
				SourceCell.LastUpdatedTick = Tick;
				MarkDirtyNeighborhood(Source);
				++Stats.CulledCells;
				return true;
			}

			if (!IsChunkActive(ToChunkCoordinate(Destination)))
			{
				return false;
			}

			FCell& DestinationCell = FindOrAddCell(Destination);
			FCell& SourceCell = FindOrAddCell(Source);
			if (DestinationCell.MaterialIndex != 0)
			{
				const FResolvedMaterial& SourceMaterial =
					Materials[SourceCell.MaterialIndex];
				const FResolvedMaterial& DestinationMaterial =
					Materials[DestinationCell.MaterialIndex];
				const bool bCanDisplace =
					DestinationMaterial.Phase !=
						EMatterFluxMaterialPhase::StaticSolid
					&& (bRising
						? SourceMaterial.Density < DestinationMaterial.Density
						: SourceMaterial.Density > DestinationMaterial.Density);
				if (!bCanDisplace)
				{
					return false;
				}
				Swap(SourceCell.MaterialIndex, DestinationCell.MaterialIndex);
				SourceCell.LastUpdatedTick = Tick;
				DestinationCell.LastUpdatedTick = Tick;
			}
			else
			{
				DestinationCell.MaterialIndex =
					SourceCell.MaterialIndex;
				DestinationCell.LastUpdatedTick = Tick;
				SourceCell.MaterialIndex = 0;
				SourceCell.LastUpdatedTick = Tick;
			}

			MarkDirtyNeighborhood(Source);
			MarkDirtyNeighborhood(Destination);
			++Stats.MovedCells;
			return true;
		}

		bool TryReactPair(
			const FIntPoint& FirstPosition,
			const FIntPoint& SecondPosition,
			FStepStats& Stats)
		{
			FCell* FirstCell = FindCell(FirstPosition);
			FCell* SecondCell = FindCell(SecondPosition);
			if (!FirstCell
				|| !SecondCell
				|| FirstCell->MaterialIndex == 0
				|| SecondCell->MaterialIndex == 0
				|| FirstCell->LastUpdatedTick == Tick
				|| SecondCell->LastUpdatedTick == Tick)
			{
				return false;
			}

			for (int32 ReactionIndex = 0;
				ReactionIndex < Reactions.Num();
				++ReactionIndex)
			{
				const FResolvedReaction& Reaction = Reactions[ReactionIndex];
				const bool bForward =
					FirstCell->MaterialIndex == Reaction.InputA
					&& SecondCell->MaterialIndex == Reaction.InputB;
				const bool bReverse =
					FirstCell->MaterialIndex == Reaction.InputB
					&& SecondCell->MaterialIndex == Reaction.InputA;
				if (!bForward && !bReverse)
				{
					continue;
				}

				const uint32 Roll = MixBits(
					Seed
					^ Tick * 0x9e3779b9u
					^ static_cast<uint32>(FirstPosition.X) * 0x85ebca6bu
					^ static_cast<uint32>(FirstPosition.Y) * 0xc2b2ae35u
					^ static_cast<uint32>(ReactionIndex));
				if (static_cast<int32>(Roll % 1000u)
					>= Reaction.ChancePermille)
				{
					return false;
				}

				FirstCell->MaterialIndex =
					bForward ? Reaction.OutputA : Reaction.OutputB;
				SecondCell->MaterialIndex =
					bForward ? Reaction.OutputB : Reaction.OutputA;
				FirstCell->LastUpdatedTick = Tick;
				SecondCell->LastUpdatedTick = Tick;
				MarkDirtyNeighborhood(FirstPosition);
				MarkDirtyNeighborhood(SecondPosition);
				++Stats.ReactedPairs;
				return true;
			}
			return false;
		}
	};

	bool FWorldSettings::IsValid() const
	{
		const int64 ActiveDiameter =
			static_cast<int64>(ActiveChunkRadius) * 2 + 1;
		return ChunkSize >= 4
			&& ChunkSize <= 256
			&& ActiveChunkRadius >= 0
			&& ActiveChunkRadius <= 16
			&& MaxActiveChunks >= ActiveDiameter * ActiveDiameter
			&& MinWorldHeightCells < MaxWorldHeightCells
			&& (!bUseSurfaceTopology
				|| (MinSurfaceCell.X < MaxSurfaceCellExclusive.X
					&& MinSurfaceCell.Y < MaxSurfaceCellExclusive.Y));
	}

	FChunkedMaterialWorld::FChunkedMaterialWorld()
		: Impl(MakeUnique<FImpl>())
	{
	}

	FChunkedMaterialWorld::~FChunkedMaterialWorld() = default;

	bool FChunkedMaterialWorld::Initialize(
		const FWorldSettings& Settings,
		const FMatterFluxContentRegistry& Registry,
		const int32 Seed,
		FString& OutError)
	{
		OutError.Reset();
		Impl = MakeUnique<FImpl>();
		if (!Settings.IsValid())
		{
			OutError = TEXT("Material world settings are invalid");
			return false;
		}
		if (Registry.Materials.IsEmpty())
		{
			OutError = TEXT("Material registry is empty");
			return false;
		}

		Impl->Settings = Settings;
		Impl->Seed = static_cast<uint32>(Seed);
		Impl->Materials.AddDefaulted();

		TArray<FName> SortedMaterialIds;
		Registry.Materials.GetKeys(SortedMaterialIds);
		SortedMaterialIds.Sort([](const FName& A, const FName& B)
		{
			return A.ToString() < B.ToString();
		});

		for (const FName MaterialId : SortedMaterialIds)
		{
			const FMatterFluxMaterialDefinition& Definition =
				Registry.Materials.FindChecked(MaterialId);
			if (MaterialId.IsNone()
				|| !FMath::IsFinite(Definition.Density)
				|| Definition.Density < 0.0f
				|| Impl->Materials.Num() > MAX_uint16)
			{
				OutError = FString::Printf(
					TEXT("Material '%s' has invalid simulation properties"),
					*MaterialId.ToString());
				Impl = MakeUnique<FImpl>();
				return false;
			}

			FImpl::FResolvedMaterial& Material =
				Impl->Materials.AddDefaulted_GetRef();
			Material.Id = MaterialId;
			Material.Phase = Definition.Phase;
			const double ScaledDensity = FMath::Clamp(
				static_cast<double>(Definition.Density) * 1000.0,
				1.0,
				static_cast<double>(MAX_uint16));
			Material.Density = static_cast<uint16>(
				FMath::RoundToInt(ScaledDensity));
			Material.Mobility = Definition.Mobility;
			Material.Dispersion = Definition.Dispersion;
			Impl->MaterialIndices.Add(
				MaterialId,
				static_cast<uint16>(Impl->Materials.Num() - 1));
		}

		TArray<FName> SortedReactionIds;
		Registry.Reactions.GetKeys(SortedReactionIds);
		SortedReactionIds.Sort([](const FName& A, const FName& B)
		{
			return A.ToString() < B.ToString();
		});
		TSet<uint32> ResolvedReactionPairs;
		for (const FName ReactionId : SortedReactionIds)
		{
			const FMatterFluxReactionDefinition& Definition =
				Registry.Reactions.FindChecked(ReactionId);
			const uint16* InputA =
				Impl->MaterialIndices.Find(Definition.InputA);
			const uint16* InputB =
				Impl->MaterialIndices.Find(Definition.InputB);
			const auto ResolveOutput =
				[&](const FName OutputId, uint16& OutIndex)
				{
					if (OutputId.IsNone() || OutputId == TEXT("empty"))
					{
						OutIndex = 0;
						return true;
					}
					const uint16* Found =
						Impl->MaterialIndices.Find(OutputId);
					if (!Found)
					{
						return false;
					}
					OutIndex = *Found;
					return true;
				};

			FImpl::FResolvedReaction Reaction;
			if (ReactionId.IsNone()
				|| !InputA
				|| !InputB
				|| !ResolveOutput(Definition.OutputA, Reaction.OutputA)
				|| !ResolveOutput(Definition.OutputB, Reaction.OutputB)
				|| Definition.ChancePermille < 0
				|| Definition.ChancePermille > 1000)
			{
				OutError = FString::Printf(
					TEXT("Reaction '%s' has invalid material references or chance"),
					*ReactionId.ToString());
				Impl = MakeUnique<FImpl>();
				return false;
			}
			const uint16 LowerInput = FMath::Min(*InputA, *InputB);
			const uint16 UpperInput = FMath::Max(*InputA, *InputB);
			const uint32 InputPairKey =
				(static_cast<uint32>(LowerInput) << 16u)
				| static_cast<uint32>(UpperInput);
			if (ResolvedReactionPairs.Contains(InputPairKey))
			{
				OutError = FString::Printf(
					TEXT("Reaction '%s' duplicates an existing unordered input pair"),
					*ReactionId.ToString());
				Impl = MakeUnique<FImpl>();
				return false;
			}
			ResolvedReactionPairs.Add(InputPairKey);
			Reaction.InputA = *InputA;
			Reaction.InputB = *InputB;
			Reaction.ChancePermille =
				static_cast<uint16>(Definition.ChancePermille);
			Impl->Reactions.Add(Reaction);
		}
		const TArray<FIntPoint> InitialFocuses = {
			FIntPoint::ZeroValue
		};
		Impl->SetFocusCells(InitialFocuses);
		Impl->bInitialized = true;
		return true;
	}

	bool FChunkedMaterialWorld::SetCell(
		const FIntPoint& WorldCell,
		const FName MaterialId)
	{
		if (!Impl->bInitialized
			|| (Impl->Settings.bUseSurfaceTopology
				? Impl->Settings.bCullOutsideSurfaceBounds
					&& !Impl->IsInsideSurfaceBounds(WorldCell)
				: Impl->Settings.bCullOutsideVerticalBounds
					&& !Impl->IsInsideVerticalBounds(WorldCell.Y)))
		{
			return false;
		}

		uint16 MaterialIndex = 0;
		if (!MaterialId.IsNone())
		{
			const uint16* FoundIndex = Impl->MaterialIndices.Find(MaterialId);
			if (!FoundIndex)
			{
				return false;
			}
			MaterialIndex = *FoundIndex;
		}

		FImpl::FCell& Cell = Impl->FindOrAddCell(WorldCell);
		Cell.MaterialIndex = MaterialIndex;
		Cell.LastUpdatedTick = 0;
		Impl->MarkDirtyNeighborhood(WorldCell);
		const FIntPoint ChunkCoordinate =
			Impl->ToChunkCoordinate(WorldCell);
		if (!Impl->IsChunkActive(ChunkCoordinate))
		{
			Impl->ArchiveChunk(ChunkCoordinate);
		}
		return true;
	}

	bool FChunkedMaterialWorld::SetSupportHeight(
		const FIntPoint& WorldCell,
		const int32 Height)
	{
		if (!Impl->bInitialized
			|| !Impl->Settings.bUseSurfaceTopology
			|| (Impl->Settings.bCullOutsideSurfaceBounds
				&& !Impl->IsInsideSurfaceBounds(WorldCell)))
		{
			return false;
		}

		FImpl::FCell& Cell = Impl->FindOrAddCell(WorldCell);
		Cell.SupportHeight = static_cast<int16>(FMath::Clamp(
			Height,
			static_cast<int32>(MIN_int16),
			static_cast<int32>(MAX_int16)));
		Impl->MarkDirtyNeighborhood(WorldCell);
		const FIntPoint ChunkCoordinate =
			Impl->ToChunkCoordinate(WorldCell);
		if (!Impl->IsChunkActive(ChunkCoordinate))
		{
			Impl->ArchiveChunk(ChunkCoordinate);
		}
		return true;
	}

	bool FChunkedMaterialWorld::SeedSurface(
		const TArray<FSeedCell>& SeedCells)
	{
		if (!Impl->bInitialized
			|| !Impl->Settings.bUseSurfaceTopology)
		{
			return false;
		}

		TSet<FIntPoint> UniqueCells;
		UniqueCells.Reserve(SeedCells.Num());
		for (const FSeedCell& SeedCell : SeedCells)
		{
			if ((Impl->Settings.bCullOutsideSurfaceBounds
					&& !Impl->IsInsideSurfaceBounds(
						SeedCell.WorldCell))
				|| UniqueCells.Contains(SeedCell.WorldCell)
				|| (!SeedCell.MaterialId.IsNone()
					&& !Impl->MaterialIndices.Contains(
						SeedCell.MaterialId)))
			{
				return false;
			}
			UniqueCells.Add(SeedCell.WorldCell);
		}

		TSet<FIntPoint> TouchedChunks;
		for (const FSeedCell& SeedCell : SeedCells)
		{
			FImpl::FCell& Cell =
				Impl->FindOrAddCell(SeedCell.WorldCell);
			Cell.MaterialIndex =
				SeedCell.MaterialId.IsNone()
					? 0
					: Impl->MaterialIndices.FindChecked(
						SeedCell.MaterialId);
			Cell.SupportHeight =
				static_cast<int16>(FMath::Clamp(
					SeedCell.SupportHeight,
					static_cast<int32>(MIN_int16),
					static_cast<int32>(MAX_int16)));
			Cell.LastUpdatedTick = 0;
			Impl->MarkDirtyNeighborhood(SeedCell.WorldCell);
			TouchedChunks.Add(
				Impl->ToChunkCoordinate(SeedCell.WorldCell));
		}

		for (const FIntPoint& ChunkCoordinate : TouchedChunks)
		{
			if (!Impl->IsChunkActive(ChunkCoordinate))
			{
				Impl->ArchiveChunk(ChunkCoordinate);
			}
		}
		return true;
	}

	FName FChunkedMaterialWorld::GetMaterialAt(
		const FIntPoint& WorldCell) const
	{
		if (!Impl->bInitialized)
		{
			return NAME_None;
		}
		const FImpl::FCell* Cell = Impl->FindCell(WorldCell);
		const uint16 MaterialIndex = Cell
			? Cell->MaterialIndex
			: Impl->FindArchivedMaterialIndex(WorldCell);
		return MaterialIndex < Impl->Materials.Num()
			? Impl->Materials[MaterialIndex].Id
			: NAME_None;
	}

	int32 FChunkedMaterialWorld::CountMaterial(const FName MaterialId) const
	{
		const uint16* MaterialIndex =
			Impl->MaterialIndices.Find(MaterialId);
		if (!MaterialIndex)
		{
			return 0;
		}

		int32 Count = 0;
		for (const TPair<FIntPoint, TUniquePtr<FImpl::FChunk>>& Pair
			: Impl->Chunks)
		{
			for (const FImpl::FCell& Cell : Pair.Value->Cells)
			{
				Count += Cell.MaterialIndex == *MaterialIndex ? 1 : 0;
			}
		}
		for (const TPair<FIntPoint, FImpl::FArchivedChunk>& Pair
			: Impl->ArchivedChunks)
		{
			for (const FImpl::FArchivedRun& Run : Pair.Value.Runs)
			{
				if (Run.MaterialIndex == *MaterialIndex)
				{
					Count += Run.Length;
				}
			}
		}
		return Count;
	}

	int32 FChunkedMaterialWorld::GetResidentChunkCount() const
	{
		return Impl->bInitialized ? Impl->Chunks.Num() : 0;
	}

	int32 FChunkedMaterialWorld::GetArchivedChunkCount() const
	{
		return Impl->bInitialized ? Impl->ArchivedChunks.Num() : 0;
	}

	int32 FChunkedMaterialWorld::GetSimulationFocusCount() const
	{
		return Impl->bInitialized ? Impl->FocusCells.Num() : 0;
	}

	void FChunkedMaterialWorld::GetActiveCells(
		TArray<FCellSnapshot>& OutCells) const
	{
		OutCells.Reset();
		if (!Impl->bInitialized)
		{
			return;
		}

		for (const TPair<FIntPoint, TUniquePtr<FImpl::FChunk>>& Pair
			: Impl->Chunks)
		{
			if (!Impl->IsChunkActive(Pair.Key))
			{
				continue;
			}
			for (int32 CellIndex = 0;
				CellIndex < Pair.Value->Cells.Num();
				++CellIndex)
			{
				const uint16 MaterialIndex =
					Pair.Value->Cells[CellIndex].MaterialIndex;
				if (MaterialIndex == 0
					|| MaterialIndex >= Impl->Materials.Num())
				{
					continue;
				}
				FCellSnapshot& Snapshot =
					OutCells.AddDefaulted_GetRef();
				Snapshot.WorldCell = FIntPoint(
					Pair.Key.X * Impl->Settings.ChunkSize
						+ CellIndex % Impl->Settings.ChunkSize,
					Pair.Key.Y * Impl->Settings.ChunkSize
						+ CellIndex / Impl->Settings.ChunkSize);
				Snapshot.MaterialId =
					Impl->Materials[MaterialIndex].Id;
				Snapshot.SupportHeight =
					Pair.Value->Cells[CellIndex].SupportHeight;
			}
		}
		OutCells.Sort([](
			const FCellSnapshot& A,
			const FCellSnapshot& B)
		{
			if (A.WorldCell.Y != B.WorldCell.Y)
			{
				return A.WorldCell.Y < B.WorldCell.Y;
			}
			return A.WorldCell.X < B.WorldCell.X;
		});
	}

	bool FChunkedMaterialWorld::ExportActiveState(
		const int32 LogicalStep,
		TArray<uint8>& OutState,
		FString& OutError) const
	{
		OutState.Reset();
		OutError.Reset();
		if (!Impl->bInitialized || LogicalStep < 0)
		{
			OutError =
				TEXT("Cannot export an uninitialized material world or a negative logical step");
			return false;
		}

		struct FOccupiedCell
		{
			uint32 CellIndex = 0;
			uint16 MaterialIndex = 0;
			int16 SupportHeight = 0;
		};
		struct FActiveChunk
		{
			FIntPoint Coordinate = FIntPoint::ZeroValue;
			TArray<FOccupiedCell> Cells;
		};
		TArray<FActiveChunk> ActiveChunks;
		for (const TPair<FIntPoint, TUniquePtr<FImpl::FChunk>>& Pair
			: Impl->Chunks)
		{
			if (!Impl->IsChunkActive(Pair.Key))
			{
				continue;
			}
			FActiveChunk Chunk;
			Chunk.Coordinate = Pair.Key;
			for (int32 CellIndex = 0;
				CellIndex < Pair.Value->Cells.Num();
				++CellIndex)
			{
				const FImpl::FCell& Cell =
					Pair.Value->Cells[CellIndex];
				if (Cell.MaterialIndex == 0)
				{
					continue;
				}
				Chunk.Cells.Add({
					static_cast<uint32>(CellIndex),
					Cell.MaterialIndex,
					Cell.SupportHeight });
			}
			if (!Chunk.Cells.IsEmpty())
			{
				ActiveChunks.Add(MoveTemp(Chunk));
			}
		}
		ActiveChunks.Sort([](
			const FActiveChunk& A,
			const FActiveChunk& B)
		{
			if (A.Coordinate.Y != B.Coordinate.Y)
			{
				return A.Coordinate.Y < B.Coordinate.Y;
			}
			return A.Coordinate.X < B.Coordinate.X;
		});

		if (Impl->FocusCells.IsEmpty()
			|| Impl->FocusCells.Num() > MAX_uint16)
		{
			OutError = TEXT("Active material state has an invalid focus count");
			return false;
		}
		OutState.Reserve(
			24 + Impl->FocusCells.Num() * 8
				+ ActiveChunks.Num() * 12);
		AppendUint32(OutState, ActiveStateMagic);
		AppendUint16(OutState, ActiveStateVersion);
		AppendUint16(
			OutState,
			static_cast<uint16>(Impl->Settings.ChunkSize));
		AppendUint16(
			OutState,
			static_cast<uint16>(
				Impl->Settings.ActiveChunkRadius));
		AppendUint16(
			OutState,
			static_cast<uint16>(Impl->FocusCells.Num()));
		AppendInt32(OutState, LogicalStep);
		AppendUint32(OutState, Impl->Tick);
		for (const FIntPoint Focus : Impl->FocusCells)
		{
			AppendInt32(OutState, Focus.X);
			AppendInt32(OutState, Focus.Y);
		}
		AppendUint32(
			OutState,
			static_cast<uint32>(ActiveChunks.Num()));
		for (const FActiveChunk& Chunk : ActiveChunks)
		{
			AppendInt32(OutState, Chunk.Coordinate.X);
			AppendInt32(OutState, Chunk.Coordinate.Y);
			AppendUint32(
				OutState,
				static_cast<uint32>(Chunk.Cells.Num()));
			for (const FOccupiedCell& Cell : Chunk.Cells)
			{
				AppendUint32(OutState, Cell.CellIndex);
				AppendUint16(OutState, Cell.MaterialIndex);
				AppendInt16(OutState, Cell.SupportHeight);
			}
			if (OutState.Num() > MaximumActiveStateBytes)
			{
				OutState.Reset();
				OutError = FString::Printf(
					TEXT("Active material state exceeds the %d-byte replication budget"),
					MaximumActiveStateBytes);
				return false;
			}
		}
		return true;
	}

	bool FChunkedMaterialWorld::ImportActiveState(
		const TArray<uint8>& State,
		int32& OutLogicalStep,
		FIntPoint& OutFocus,
		FString& OutError)
	{
		OutLogicalStep = INDEX_NONE;
		OutFocus = FIntPoint::ZeroValue;
		OutError.Reset();
		if (!Impl->bInitialized
			|| State.IsEmpty()
			|| State.Num() > MaximumActiveStateBytes)
		{
			OutError =
				TEXT("Active material state is empty, oversized, or the destination world is uninitialized");
			return false;
		}

		struct FImportedCell
		{
			uint32 CellIndex = 0;
			uint16 MaterialIndex = 0;
			int16 SupportHeight = 0;
		};
		struct FImportedChunk
		{
			FIntPoint Coordinate = FIntPoint::ZeroValue;
			TArray<FImportedCell> Cells;
		};

		int32 Offset = 0;
		uint32 Magic = 0;
		uint16 Version = 0;
		uint16 ChunkSize = 0;
		uint16 ActiveRadius = 0;
		uint16 FocusCountOrReserved = 0;
		int32 LogicalStep = INDEX_NONE;
		uint32 SimulationTick = 0;
		TArray<FIntPoint> Focuses;
		uint32 ChunkCount = 0;
		if (!ReadUint32(State, Offset, Magic)
			|| !ReadUint16(State, Offset, Version)
			|| !ReadUint16(State, Offset, ChunkSize)
			|| !ReadUint16(State, Offset, ActiveRadius)
			|| !ReadUint16(State, Offset, FocusCountOrReserved)
			|| !ReadInt32(State, Offset, LogicalStep)
			|| !ReadUint32(State, Offset, SimulationTick)
			|| Magic != ActiveStateMagic
			|| (Version != LegacyActiveStateVersion
				&& Version != ActiveStateVersion)
			|| ChunkSize != Impl->Settings.ChunkSize
			|| ActiveRadius
				!= Impl->Settings.ActiveChunkRadius
			|| LogicalStep < 0)
		{
			OutError =
				TEXT("Active material state header is invalid or incompatible");
			return false;
		}

		const uint16 FocusCount =
			Version == LegacyActiveStateVersion
				? 1
				: FocusCountOrReserved;
		if ((Version == LegacyActiveStateVersion
				&& FocusCountOrReserved != 0)
			|| FocusCount == 0
			|| FocusCount
				> static_cast<uint16>(FMath::Min(
					Impl->Settings.MaxActiveChunks,
					static_cast<int32>(MAX_uint16))))
		{
			OutError = TEXT("Active material state focus list is invalid");
			return false;
		}
		Focuses.Reserve(FocusCount);
		for (uint16 FocusIndex = 0;
			FocusIndex < FocusCount;
			++FocusIndex)
		{
			FIntPoint& Focus = Focuses.AddDefaulted_GetRef();
			if (!ReadInt32(State, Offset, Focus.X)
				|| !ReadInt32(State, Offset, Focus.Y))
			{
				OutError = TEXT("Active material state focus list is truncated");
				return false;
			}
		}
		if (!ReadUint32(State, Offset, ChunkCount)
			|| ChunkCount
				> static_cast<uint32>(
					Impl->Settings.MaxActiveChunks))
		{
			OutError = TEXT("Active material state chunk count is invalid");
			return false;
		}

		TArray<FIntPoint> CanonicalFocuses;
		TSet<FIntPoint> ExpectedActiveChunks;
		Impl->BuildFocusState(
			Focuses,
			CanonicalFocuses,
			ExpectedActiveChunks);
		if (CanonicalFocuses != Focuses)
		{
			OutError = TEXT("Active material state focus order is not canonical");
			return false;
		}

		const int32 CellCount =
			Impl->Settings.ChunkSize
			* Impl->Settings.ChunkSize;
		TArray<FImportedChunk> ImportedChunks;
		ImportedChunks.Reserve(static_cast<int32>(ChunkCount));
		FIntPoint PreviousChunk(MIN_int32, MIN_int32);
		for (uint32 ChunkIndex = 0;
			ChunkIndex < ChunkCount;
			++ChunkIndex)
		{
			FImportedChunk Chunk;
			uint32 OccupiedCellCount = 0;
			if (!ReadInt32(
					State,
					Offset,
					Chunk.Coordinate.X)
				|| !ReadInt32(
					State,
					Offset,
					Chunk.Coordinate.Y)
				|| !ReadUint32(
					State,
					Offset,
					OccupiedCellCount)
				|| OccupiedCellCount
					> static_cast<uint32>(CellCount)
				|| !ExpectedActiveChunks.Contains(
					Chunk.Coordinate)
				|| (ChunkIndex > 0
					&& (Chunk.Coordinate.Y < PreviousChunk.Y
						|| (Chunk.Coordinate.Y
								== PreviousChunk.Y
							&& Chunk.Coordinate.X
								<= PreviousChunk.X))))
			{
				OutError =
					TEXT("Active material state contains an invalid or duplicate chunk");
				return false;
			}
			PreviousChunk = Chunk.Coordinate;
			Chunk.Cells.Reserve(
				static_cast<int32>(OccupiedCellCount));
			uint32 PreviousCellIndex = MAX_uint32;
			for (uint32 CellNumber = 0;
				CellNumber < OccupiedCellCount;
				++CellNumber)
			{
				FImportedCell Cell;
				if (!ReadUint32(
						State,
						Offset,
						Cell.CellIndex)
					|| !ReadUint16(
						State,
						Offset,
						Cell.MaterialIndex)
					|| !ReadInt16(
						State,
						Offset,
						Cell.SupportHeight)
					|| Cell.CellIndex
						>= static_cast<uint32>(CellCount)
					|| Cell.MaterialIndex == 0
					|| Cell.MaterialIndex
						>= Impl->Materials.Num()
					|| (CellNumber > 0
						&& Cell.CellIndex
							<= PreviousCellIndex))
				{
					OutError =
						TEXT("Active material state contains an invalid occupied cell");
					return false;
				}
				PreviousCellIndex = Cell.CellIndex;
				Chunk.Cells.Add(Cell);
			}
			ImportedChunks.Add(MoveTemp(Chunk));
		}
		if (Offset != State.Num())
		{
			OutError =
				TEXT("Active material state contains trailing or truncated data");
			return false;
		}

		SetSimulationFocuses(Focuses);
		for (const TPair<FIntPoint, TUniquePtr<FImpl::FChunk>>& Pair
			: Impl->Chunks)
		{
			if (!Impl->IsChunkActive(Pair.Key))
			{
				continue;
			}
			for (FImpl::FCell& Cell : Pair.Value->Cells)
			{
				Cell.MaterialIndex = 0;
				Cell.LastUpdatedTick = 0;
			}
			Pair.Value->DirtyMin = FIntPoint(0, 0);
			Pair.Value->DirtyMax = FIntPoint(
				Impl->Settings.ChunkSize - 1,
				Impl->Settings.ChunkSize - 1);
			Pair.Value->bHasDirtyCells = true;
		}
		for (const FImportedChunk& ImportedChunk
			: ImportedChunks)
		{
			FImpl::FChunk& Chunk =
				Impl->FindOrAddChunk(
					ImportedChunk.Coordinate);
			for (const FImportedCell& ImportedCell
				: ImportedChunk.Cells)
			{
				FImpl::FCell& Cell =
					Chunk.Cells[
						static_cast<int32>(
							ImportedCell.CellIndex)];
				Cell.MaterialIndex =
					ImportedCell.MaterialIndex;
				Cell.SupportHeight =
					ImportedCell.SupportHeight;
				Cell.LastUpdatedTick = 0;
			}
			Chunk.DirtyMin = FIntPoint(0, 0);
			Chunk.DirtyMax = FIntPoint(
				Impl->Settings.ChunkSize - 1,
				Impl->Settings.ChunkSize - 1);
			Chunk.bHasDirtyCells = true;
		}
		Impl->Tick = SimulationTick;
		OutLogicalStep = LogicalStep;
		OutFocus = Focuses[0];
		return true;
	}

	void FChunkedMaterialWorld::SetSimulationFocus(
		const FIntPoint& WorldCell)
	{
		const TArray<FIntPoint> Focuses = { WorldCell };
		SetSimulationFocuses(Focuses);
	}

	void FChunkedMaterialWorld::SetSimulationFocuses(
		const TConstArrayView<FIntPoint> WorldCells)
	{
		if (!Impl->bInitialized)
		{
			return;
		}
		const TSet<FIntPoint> PreviousActiveChunks = Impl->ActiveChunks;
		Impl->SetFocusCells(WorldCells);
		bool bActiveSetChanged =
			PreviousActiveChunks.Num() != Impl->ActiveChunks.Num();
		if (!bActiveSetChanged)
		{
			for (const FIntPoint ChunkCoordinate : PreviousActiveChunks)
			{
				if (!Impl->ActiveChunks.Contains(ChunkCoordinate))
				{
					bActiveSetChanged = true;
					break;
				}
			}
		}
		if (!bActiveSetChanged)
		{
			return;
		}

		TArray<FIntPoint> ChunksToArchive;
		for (const TPair<FIntPoint, TUniquePtr<FImpl::FChunk>>& Pair
			: Impl->Chunks)
		{
			if (!Impl->IsChunkActive(Pair.Key))
			{
				ChunksToArchive.Add(Pair.Key);
			}
		}
		for (const FIntPoint& ChunkCoordinate : ChunksToArchive)
		{
			Impl->ArchiveChunk(ChunkCoordinate);
		}

		TArray<FIntPoint> ChunksToRestore;
		for (const TPair<FIntPoint, FImpl::FArchivedChunk>& Pair
			: Impl->ArchivedChunks)
		{
			if (Impl->IsChunkActive(Pair.Key))
			{
				ChunksToRestore.Add(Pair.Key);
			}
		}
		for (const FIntPoint& ChunkCoordinate : ChunksToRestore)
		{
			FImpl::FChunk& Chunk =
				Impl->FindOrAddChunk(ChunkCoordinate);
			Chunk.DirtyMin = FIntPoint(0, 0);
			Chunk.DirtyMax = FIntPoint(
				Impl->Settings.ChunkSize - 1,
				Impl->Settings.ChunkSize - 1);
			Chunk.bHasDirtyCells = true;
		}

		for (const TPair<FIntPoint, TUniquePtr<FImpl::FChunk>>& Pair
			: Impl->Chunks)
		{
			if (!Impl->IsChunkActive(Pair.Key)
				|| !PreviousActiveChunks.Contains(Pair.Key))
			{
				continue;
			}

			// Retained chunks only need to retry cells along a seam whose
			// neighbor has just entered the active window. Newly restored
			// chunks above are already marked fully dirty.
			for (int32 NeighborY = -1; NeighborY <= 1; ++NeighborY)
			{
				for (int32 NeighborX = -1;
					NeighborX <= 1;
					++NeighborX)
				{
					if (NeighborX == 0 && NeighborY == 0)
					{
						continue;
					}
					const FIntPoint NeighborChunk =
						Pair.Key + FIntPoint(
							NeighborX,
							NeighborY);
					if (!Impl->IsChunkActive(NeighborChunk)
						|| PreviousActiveChunks.Contains(NeighborChunk))
					{
						continue;
					}

					const int32 MinX = NeighborX < 0
						? 0
						: NeighborX > 0
							? Impl->Settings.ChunkSize - 1
							: 0;
					const int32 MaxX = NeighborX == 0
						? Impl->Settings.ChunkSize - 1
						: MinX;
					const int32 MinY = NeighborY < 0
						? 0
						: NeighborY > 0
							? Impl->Settings.ChunkSize - 1
							: 0;
					const int32 MaxY = NeighborY == 0
						? Impl->Settings.ChunkSize - 1
						: MinY;
					for (int32 LocalY = MinY;
						LocalY <= MaxY;
						++LocalY)
					{
						for (int32 LocalX = MinX;
							LocalX <= MaxX;
							++LocalX)
						{
							Pair.Value->MarkDirty(
								FIntPoint(LocalX, LocalY));
						}
					}
				}
			}
		}
	}

	FStepStats FChunkedMaterialWorld::Step()
	{
		FStepStats Stats;
		if (!Impl->bInitialized)
		{
			return Stats;
		}
		++Impl->Tick;
		if (Impl->Tick == 0)
		{
			++Impl->Tick;
		}

		TArray<FIntPoint> CellsToVisit;
		for (const TPair<FIntPoint, TUniquePtr<FImpl::FChunk>>& Pair
			: Impl->Chunks)
		{
			FImpl::FChunk& Chunk = *Pair.Value;
			if (!Impl->IsChunkActive(Pair.Key) || !Chunk.bHasDirtyCells)
			{
				continue;
			}

			const FIntPoint DirtyMin = Chunk.DirtyMin;
			const FIntPoint DirtyMax = Chunk.DirtyMax;
			Chunk.bHasDirtyCells = false;
			Chunk.DirtyMin = FIntPoint(MAX_int32, MAX_int32);
			Chunk.DirtyMax = FIntPoint(MIN_int32, MIN_int32);
			for (int32 LocalY = DirtyMin.Y; LocalY <= DirtyMax.Y; ++LocalY)
			{
				for (int32 LocalX = DirtyMin.X; LocalX <= DirtyMax.X; ++LocalX)
				{
					CellsToVisit.Add(FIntPoint(
						Pair.Key.X * Impl->Settings.ChunkSize + LocalX,
						Pair.Key.Y * Impl->Settings.ChunkSize + LocalY));
				}
			}
		}
		CellsToVisit.Sort([Tick = Impl->Tick](
			const FIntPoint& A,
			const FIntPoint& B)
		{
			if (A.Y != B.Y)
			{
				return A.Y < B.Y;
			}
			return (Tick & 1u) == 0 ? A.X < B.X : A.X > B.X;
		});

		for (const FIntPoint& WorldCell : CellsToVisit)
		{
			const FImpl::FCell* Cell = Impl->FindCell(WorldCell);
			if (!Cell
				|| Cell->MaterialIndex == 0
				|| Cell->LastUpdatedTick == Impl->Tick)
			{
				continue;
			}
			FIntPoint Neighbor;
			if (TryOffsetCell(
					WorldCell,
					FIntPoint(1, 0),
					Neighbor)
				&& Impl->TryReactPair(
					WorldCell,
					Neighbor,
					Stats))
			{
				continue;
			}
			if (TryOffsetCell(
				WorldCell,
				FIntPoint(0, 1),
				Neighbor))
			{
				Impl->TryReactPair(
					WorldCell,
					Neighbor,
					Stats);
			}
		}

		for (const FIntPoint& WorldCell : CellsToVisit)
		{
			FImpl::FCell* Cell = Impl->FindCell(WorldCell);
			if (!Cell
				|| Cell->MaterialIndex == 0
				|| Cell->LastUpdatedTick == Impl->Tick)
			{
				continue;
			}
			++Stats.VisitedCells;
			const FImpl::FResolvedMaterial Material =
				Impl->Materials[Cell->MaterialIndex];
			const bool bIsLiquid =
				Material.Phase == EMatterFluxMaterialPhase::Liquid;
			const bool bIsPowder =
				Material.Phase == EMatterFluxMaterialPhase::Powder;
			const bool bIsGas =
				Material.Phase == EMatterFluxMaterialPhase::Gas;
			if (!bIsLiquid && !bIsPowder && !bIsGas)
			{
				continue;
			}
			if (!Impl->PassesMobilityCheck(WorldCell, Material))
			{
				Impl->MarkDirty(WorldCell);
				continue;
			}
			if (Impl->Settings.bUseSurfaceTopology)
			{
				Impl->TryMoveSurfaceCell(
					WorldCell,
					Material,
					Stats);
				continue;
			}

			const uint32 DirectionHash = MixBits(
				Impl->Seed
				^ static_cast<uint32>(WorldCell.X)
				^ (static_cast<uint32>(WorldCell.Y) << 16u)
				^ Impl->Tick);
			const int32 FirstDirection =
				(DirectionHash & 1u) == 0 ? -1 : 1;
			const auto TryMoveOffset =
				[&](
					const FIntPoint& Offset,
					const bool bRising = false)
				{
					FIntPoint Destination;
					return TryOffsetCell(
							WorldCell,
							Offset,
							Destination)
						&& Impl->TryMoveCell(
							WorldCell,
							Destination,
							Stats,
							bRising);
				};
			if (bIsGas)
			{
				if (TryMoveOffset(FIntPoint(0, 1), true))
				{
					continue;
				}
				if (TryMoveOffset(
					FIntPoint(FirstDirection, 1),
					true))
				{
					continue;
				}
				if (TryMoveOffset(
					FIntPoint(-FirstDirection, 1),
					true))
				{
					continue;
				}
				if (static_cast<uint8>((DirectionHash >> 8u) & 0xffu)
					< Material.Dispersion)
				{
					if (TryMoveOffset(
						FIntPoint(FirstDirection, 0),
						true))
					{
						continue;
					}
					TryMoveOffset(
						FIntPoint(-FirstDirection, 0),
						true);
				}
				continue;
			}

			if (TryMoveOffset(FIntPoint(0, -1)))
			{
				continue;
			}

			if (TryMoveOffset(
				FIntPoint(FirstDirection, -1)))
			{
				continue;
			}
			if (TryMoveOffset(
				FIntPoint(-FirstDirection, -1)))
			{
				continue;
			}
			if (bIsPowder)
			{
				continue;
			}
			if (TryMoveOffset(
				FIntPoint(FirstDirection, 0)))
			{
				continue;
			}
			TryMoveOffset(
				FIntPoint(-FirstDirection, 0));
		}
		return Stats;
	}
}
