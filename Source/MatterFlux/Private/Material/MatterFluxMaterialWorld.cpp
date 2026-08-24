#include "Material/MatterFluxMaterialWorld.h"
#include "Material/MatterFluxMaterialReactionEngine.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace MatterFlux::Material
{
	namespace
	{
		constexpr uint32 ActiveStateMagic = 0x3153464du;
		constexpr uint16 LegacyActiveStateVersion = 1;
		constexpr uint16 PreviousActiveStateVersion = 2;
		constexpr uint16 CompactActiveStateVersion = 3;
		constexpr uint16 BaselineDeltaActiveStateVersion = 4;
		constexpr uint16 ActiveStateVersion = 7;
		constexpr int32 MaximumActiveStateBytes = 1024 * 1024;
		constexpr uint16 FullCellAmount = 255;
		/** World-space pile height represented by one full powder unit. */
		constexpr int32 PowderFullColumnHeight = 14;
		constexpr uint8 MinimumLiquidEdgeAmount = 24;
		constexpr int32 MaximumLiquidTransferPerStep = 72;
		// A vacancy is a canonical, short-lived body/material interaction fact.
		// Runtime creatures can traverse a large basin for hundreds of simulation
		// steps before their earliest wake is free to settle. Expiring after 64
		// steps discarded that fact mid-traversal and left permanent empty/low
		// columns on uneven terrain. This remains bounded, but spans a complete
		// long traversal and its restitution window.
		constexpr uint32 MaximumBodyDisplacementRefillTicks = 512;
		constexpr uint16 MaterialIndexMask = 0x7fffu;

		void AppendUint8(TArray<uint8>& Bytes, const uint8 Value)
		{
			Bytes.Add(Value);
		}

		void AppendVarUint32(TArray<uint8>& Bytes, uint32 Value)
		{
			do
			{
				uint8 Byte = static_cast<uint8>(Value & 0x7fu);
				Value >>= 7u;
				if (Value != 0)
				{
					Byte |= 0x80u;
				}
				Bytes.Add(Byte);
			}
			while (Value != 0);
		}

		int32 VarUint32ByteCount(uint32 Value)
		{
			int32 Count = 1;
			while (Value >= 0x80u)
			{
				Value >>= 7u;
				++Count;
			}
			return Count;
		}

		uint32 ZigZagEncodeInt32(const int32 Value)
		{
			return (static_cast<uint32>(Value) << 1u)
				^ static_cast<uint32>(Value >> 31);
		}

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

		bool ReadUint8(
			const TArray<uint8>& Bytes,
			int32& Offset,
			uint8& OutValue)
		{
			if (Offset < 0 || Offset >= Bytes.Num())
			{
				return false;
			}
			OutValue = Bytes[Offset++];
			return true;
		}

		bool ReadVarUint32(
			const TArray<uint8>& Bytes,
			int32& Offset,
			uint32& OutValue)
		{
			OutValue = 0;
			for (int32 ByteIndex = 0; ByteIndex < 5; ++ByteIndex)
			{
				uint8 Byte = 0;
				if (!ReadUint8(Bytes, Offset, Byte)
					|| (ByteIndex == 4 && (Byte & 0xf0u) != 0))
				{
					return false;
				}
				OutValue |= static_cast<uint32>(Byte & 0x7fu)
					<< (ByteIndex * 7);
				if ((Byte & 0x80u) == 0)
				{
					return true;
				}
			}
			return false;
		}

		int32 ZigZagDecodeInt32(const uint32 Value)
		{
			return static_cast<int32>(
				(Value >> 1u) ^ (0u - (Value & 1u)));
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
		struct FSurfacePuddleShape
		{
			FVector2D Center = FVector2D::ZeroVector;
			float Radius = 0.0f;
			int8 AxisBias = 0;
		};

		struct FResolvedMaterial
		{
			FName Id = NAME_None;
			EMatterFluxMaterialPhase Phase =
				EMatterFluxMaterialPhase::StaticSolid;
			uint16 Density = 1;
			uint8 Mobility = 255;
			uint8 Dispersion = 128;
			uint8 LifetimeSteps = 0;
		};

		struct FCell
		{
			uint16 MaterialIndex = 0;
			int16 SupportHeight = 0;
			uint16 Amount = 0;
			uint8 RemainingLifetime = 0;
			uint32 LastUpdatedTick = 0;
			// A transient body displaced volume from this world column. This is
			// location state, not material state: it must not travel with liquid.
			// The hydraulic solver uses it briefly to refill across uneven terrain
			// without globally connecting authored rivers and lakes by elevation.
			uint32 BodyDisplacedTick = 0;
			uint16 BodyDisplacedMaterialIndex = 0;
			uint8 BodyDisplacedReferenceAmount = 0;
			bool bHasBodyDisplacementVacancy = false;
		};

		struct FResolvedReaction
		{
			FMatterFluxReactionDefinition Rule;
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
			uint16 Amount = 0;
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
		/** Deterministic generated-map seed used as the replication baseline. */
		TMap<FIntPoint, FArchivedChunk> BaselineChunks;
		/** Sparse transient columns whose liquid volume was constrained by bodies. */
		TSet<FIntPoint> BodyDisplacementVacancies;
		/** Chunks whose canonical facts changed since the last render projection. */
		TSet<FIntPoint> ProjectionDirtyChunks;
		FLiquidDisplacementStats LastLiquidDisplacementStats;
		FIntPoint FocusCell = FIntPoint::ZeroValue;
		TArray<FIntPoint> FocusCells;
		TSet<FIntPoint> ActiveChunks;
		/**
		 * Surface chunks temporarily kept resident while liquid crosses the
		 * player-focused simulation window. They retire as soon as their dirty
		 * work settles, so a streaming seam can never act as physical terrain.
		 */
		TSet<FIntPoint> SurfaceFlowChunks;
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

		bool IsChunkSimulated(const FIntPoint& ChunkCoordinate) const
		{
			return IsChunkActive(ChunkCoordinate)
				|| (Settings.bUseSurfaceTopology
					&& SurfaceFlowChunks.Contains(ChunkCoordinate));
		}

		void WakeSurfaceFlow(const FIntPoint& WorldCell)
		{
			if (!Settings.bUseSurfaceTopology
				|| (Settings.bCullOutsideSurfaceBounds
					&& !IsInsideSurfaceBounds(WorldCell)))
			{
				return;
			}
			SurfaceFlowChunks.Add(ToChunkCoordinate(WorldCell));
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
			uint16 CurrentAmount = Chunk.Cells[0].Amount;
			uint16 CurrentLength = 0;
			for (const FCell& Cell : Chunk.Cells)
			{
				if (Cell.MaterialIndex != CurrentMaterial
					|| Cell.SupportHeight != CurrentSupport
					|| Cell.Amount != CurrentAmount
					|| CurrentLength == MAX_uint16)
				{
					Archive.Runs.Add({
						CurrentMaterial,
						CurrentSupport,
						CurrentAmount,
						CurrentLength });
					CurrentMaterial = Cell.MaterialIndex;
					CurrentSupport = Cell.SupportHeight;
					CurrentAmount = Cell.Amount;
					CurrentLength = 0;
				}
				++CurrentLength;
			}
			Archive.Runs.Add({
				CurrentMaterial,
				CurrentSupport,
				CurrentAmount,
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
					OutChunk.Cells[CellIndex].Amount = Run.Amount;
					OutChunk.Cells[CellIndex].RemainingLifetime = 0;
					OutChunk.Cells[CellIndex].LastUpdatedTick = 0;
					OutChunk.Cells[CellIndex].BodyDisplacedTick = 0;
					OutChunk.Cells[CellIndex].BodyDisplacedMaterialIndex = 0;
					OutChunk.Cells[CellIndex].BodyDisplacedReferenceAmount = 0;
					OutChunk.Cells[CellIndex].bHasBodyDisplacementVacancy = false;
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
			bool bDiscardedTransientMaterial = false;
			for (FCell& Cell : (*Chunk)->Cells)
			{
				if (Cell.RemainingLifetime == 0)
				{
					continue;
				}
				Cell.MaterialIndex = 0;
				Cell.Amount = 0;
				Cell.RemainingLifetime = 0;
				bDiscardedTransientMaterial = true;
			}
			if (bDiscardedTransientMaterial)
			{
				ProjectionDirtyChunks.Add(ChunkCoordinate);
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

		uint16 FindArchivedAmount(const FIntPoint& WorldCell) const
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
					return Run.Amount;
				}
				RemainingIndex -= Run.Length;
			}
			return 0;
		}

		bool FindBaselineCell(
			const FIntPoint& WorldCell,
			uint16& OutMaterialIndex,
			int16& OutSupportHeight,
			uint16& OutAmount) const
		{
			OutMaterialIndex = 0;
			OutSupportHeight = 0;
			OutAmount = 0;
			const FArchivedChunk* Baseline =
				BaselineChunks.Find(ToChunkCoordinate(WorldCell));
			if (!Baseline)
			{
				return false;
			}
			int32 RemainingIndex = ToIndex(ToLocalCoordinate(WorldCell));
			for (const FArchivedRun& Run : Baseline->Runs)
			{
				if (RemainingIndex < Run.Length)
				{
					OutMaterialIndex = Run.MaterialIndex;
					OutSupportHeight = Run.SupportHeight;
					OutAmount = Run.Amount;
					return true;
				}
				RemainingIndex -= Run.Length;
			}
			return false;
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

		bool HasConfiguredReactionPair(
			const uint16 FirstMaterialIndex,
			const uint16 SecondMaterialIndex) const
		{
			if (FirstMaterialIndex == 0
				|| SecondMaterialIndex == 0
				|| FirstMaterialIndex >= Materials.Num()
				|| SecondMaterialIndex >= Materials.Num())
			{
				return false;
			}
			const FName FirstId = Materials[FirstMaterialIndex].Id;
			const FName SecondId = Materials[SecondMaterialIndex].Id;
			return Reactions.ContainsByPredicate(
				[FirstId, SecondId](const FResolvedReaction& Reaction)
				{
					return (Reaction.Rule.InputA == FirstId
							&& Reaction.Rule.InputB == SecondId)
						|| (Reaction.Rule.InputA == SecondId
							&& Reaction.Rule.InputB == FirstId);
				});
		}

		void MarkChunkForBaselineResume(
			const FIntPoint& ChunkCoordinate)
		{
			TUniquePtr<FChunk>* FoundChunk = Chunks.Find(ChunkCoordinate);
			if (!FoundChunk || !FoundChunk->IsValid())
			{
				return;
			}
			FChunk& Chunk = **FoundChunk;
			Chunk.bHasDirtyCells = false;
			Chunk.DirtyMin = FIntPoint(MAX_int32, MAX_int32);
			Chunk.DirtyMax = FIntPoint(MIN_int32, MIN_int32);
			if (BaselineChunks.IsEmpty())
			{
				Chunk.DirtyMin = FIntPoint::ZeroValue;
				Chunk.DirtyMax = FIntPoint(
					Settings.ChunkSize - 1,
					Settings.ChunkSize - 1);
				Chunk.bHasDirtyCells = true;
				return;
			}

			static const FIntPoint ReactionOffsets[] = {
				FIntPoint(1, 0),
				FIntPoint(-1, 0),
				FIntPoint(0, 1),
				FIntPoint(0, -1)
			};
			for (int32 CellIndex = 0;
				CellIndex < Chunk.Cells.Num();
				++CellIndex)
			{
				const FCell& Cell = Chunk.Cells[CellIndex];
				const FIntPoint WorldCell(
					ChunkCoordinate.X * Settings.ChunkSize
						+ CellIndex % Settings.ChunkSize,
					ChunkCoordinate.Y * Settings.ChunkSize
						+ CellIndex / Settings.ChunkSize);
				uint16 BaselineMaterial = 0;
				int16 BaselineSupport = 0;
				uint16 BaselineAmount = 0;
				FindBaselineCell(
					WorldCell,
					BaselineMaterial,
					BaselineSupport,
					BaselineAmount);
				bool bNeedsWake = Cell.MaterialIndex != BaselineMaterial
					|| Cell.SupportHeight != BaselineSupport
					|| Cell.Amount != BaselineAmount;
				if (!bNeedsWake
					&& Cell.MaterialIndex != 0
					&& Cell.MaterialIndex < Materials.Num())
				{
					const EMatterFluxMaterialPhase Phase =
						Materials[Cell.MaterialIndex].Phase;
					bNeedsWake = Phase == EMatterFluxMaterialPhase::Powder
						|| Phase == EMatterFluxMaterialPhase::Gas;
					for (const FIntPoint Offset : ReactionOffsets)
					{
						if (bNeedsWake)
						{
							break;
						}
						FIntPoint Neighbor;
						if (!TryOffsetCell(WorldCell, Offset, Neighbor))
						{
							continue;
						}
						const FCell* NeighborCell = FindCell(Neighbor);
						const uint16 NeighborMaterial = NeighborCell
							? NeighborCell->MaterialIndex
							: FindArchivedMaterialIndex(Neighbor);
						bNeedsWake = HasConfiguredReactionPair(
							Cell.MaterialIndex, NeighborMaterial);
					}
				}
				if (bNeedsWake)
				{
					MarkDirtyNeighborhood(WorldCell);
				}
			}
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
			if (!IsChunkSimulated(ChunkCoordinate))
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
						ProjectionDirtyChunks.Add(
							ToChunkCoordinate(Neighbor));
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
			FStepStats& Stats,
			const FSurfacePuddleShape* PuddleShape)
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

			if (Material.Phase == EMatterFluxMaterialPhase::Powder)
			{
				FCell* SourceCell = FindCell(Source);
				if (!SourceCell || SourceCell->Amount == 0)
				{
					return false;
				}

				// Surface powder is a conserved height column. It flows toward the
				// lowest neighboring powder surface until adjacent columns differ by
				// at most one full grain layer, which produces a stable angle of
				// repose instead of a disk of identical-height tiles.
				const int64 SourceSurface255 =
					static_cast<int64>(SourceSupport) * FullCellAmount
						+ static_cast<int64>(PowderFullColumnHeight)
							* SourceCell->Amount;
				int64 LowestSurface255 = SourceSurface255;
				FIntPoint LowestDestination = Source;
				uint16 LowestAmount = 0;
				for (int32 Offset = 0;
					Offset < UE_ARRAY_COUNT(Directions);
					++Offset)
				{
					FIntPoint Destination;
					if (!TryOffsetCell(
							Source,
							Directions[(FirstDirection + Offset)
								% UE_ARRAY_COUNT(Directions)],
							Destination)
						|| (Settings.bCullOutsideSurfaceBounds
							&& !IsInsideSurfaceBounds(Destination)))
					{
						continue;
					}

					int16 DestinationSupport = 0;
					if (!FindSupportHeight(Destination, DestinationSupport))
					{
						continue;
					}
					const FCell* DestinationCell = FindCell(Destination);
					const uint16 DestinationMaterialIndex = DestinationCell
						? DestinationCell->MaterialIndex
						: FindArchivedMaterialIndex(Destination);
					if (DestinationMaterialIndex != 0
						&& DestinationMaterialIndex != SourceCell->MaterialIndex)
					{
						continue;
					}
					const uint16 DestinationAmount = DestinationCell
						? DestinationCell->Amount
						: FindArchivedAmount(Destination);
					const int64 DestinationSurface255 =
						static_cast<int64>(DestinationSupport) * FullCellAmount
							+ static_cast<int64>(PowderFullColumnHeight)
								* DestinationAmount;
					if (DestinationSurface255 < LowestSurface255)
					{
						LowestSurface255 = DestinationSurface255;
						LowestDestination = Destination;
						LowestAmount = DestinationAmount;
					}
				}

				constexpr int64 MaximumStableSlope255 =
					static_cast<int64>(PowderFullColumnHeight)
						* FullCellAmount;
				const int64 ExcessSurface255 =
					SourceSurface255 - LowestSurface255
						- MaximumStableSlope255;
				if (LowestDestination == Source || ExcessSurface255 <= 0)
				{
					return false;
				}

				const int32 RequestedTransfer = FMath::Max<int32>(
					static_cast<int32>(
						ExcessSurface255
							/ (2 * PowderFullColumnHeight)),
					1);
				const int32 TransferAmount = FMath::Min3(
					RequestedTransfer,
					static_cast<int32>(SourceCell->Amount),
					static_cast<int32>(MAX_uint16 - LowestAmount));
				if (TransferAmount <= 0)
				{
					return false;
				}

				WakeSurfaceFlow(LowestDestination);
				FCell& DestinationCell = FindOrAddCell(LowestDestination);
				SourceCell = FindCell(Source);
				DestinationCell.MaterialIndex = SourceCell->MaterialIndex;
				DestinationCell.Amount = static_cast<uint16>(
					DestinationCell.Amount + TransferAmount);
				DestinationCell.RemainingLifetime =
					SourceCell->RemainingLifetime;
				SourceCell->Amount = static_cast<uint16>(
					SourceCell->Amount - TransferAmount);
				if (SourceCell->Amount == 0)
				{
					SourceCell->MaterialIndex = 0;
					SourceCell->RemainingLifetime = 0;
				}
				DestinationCell.LastUpdatedTick = Tick;
				SourceCell->LastUpdatedTick = Tick;
				MarkDirtyNeighborhood(Source);
				MarkDirtyNeighborhood(LowestDestination);
				++Stats.MovedCells;
				return true;
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
						DestinationSupport))
				{
					continue;
				}
				const bool bDestinationOccupied =
					DestinationCell
					&& DestinationCell->MaterialIndex != 0;
				if (Material.Phase == EMatterFluxMaterialPhase::Liquid
					&& !bDestinationOccupied)
				{
					// Empty surface-topology columns are filled by the hydraulic
					// free-surface transfer below at every terrain height. Moving a
					// whole liquid column downhill bypasses conserved partial flow and
					// can repeatedly empty a just-restored body wake.
					continue;
				}
				bool bCanDisplaceLighterLiquid = false;
				if (bDestinationOccupied
					&& Material.Phase == EMatterFluxMaterialPhase::Liquid
					&& DestinationSupport < SourceSupport)
				{
					const FResolvedMaterial& DestinationMaterial =
						Materials[DestinationCell->MaterialIndex];
					bCanDisplaceLighterLiquid =
						DestinationMaterial.Phase
							== EMatterFluxMaterialPhase::Liquid
						&& Material.Density > DestinationMaterial.Density;
				}
				if ((bDestinationOccupied && !bCanDisplaceLighterLiquid)
					|| DestinationSupport >= SourceSupport)
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
			if (bFoundDestination)
			{
				return TryMoveCell(
					Source,
					BestDestination,
					Stats);
			}

			if (Material.Phase != EMatterFluxMaterialPhase::Liquid)
			{
				return false;
			}

			// 平地上的液体不是整格随机游走，而是按格内液量均衡。
			// 世界种子只改变低液量边缘的表面张力阈值，因此相同种子
			// 完全确定，同时水洼边界不会成为规则矩形。
			const FCell* SourceCell = FindCell(Source);
			if (!SourceCell || SourceCell->Amount == 0)
			{
				return false;
			}
			int32 MinimumRetainedAmount =
				SourceCell->bHasBodyDisplacementVacancy
					? SourceCell->BodyDisplacedReferenceAmount
					: 0;
			if (MinimumRetainedAmount == 0
				&& !BodyDisplacementVacancies.IsEmpty())
			{
				uint16 BaselineMaterialIndex = 0;
				int16 BaselineSupportHeight = 0;
				uint16 BaselineAmount = 0;
				if (FindBaselineCell(
						Source,
						BaselineMaterialIndex,
						BaselineSupportHeight,
						BaselineAmount)
					&& BaselineMaterialIndex == SourceCell->MaterialIndex)
				{
					// While any body/material transaction is active, authored lake
					// columns may move only the displaced surplus above their own
					// particle amount. Otherwise ordinary pressure flow can relocate a
					// wake into an unrelated donor column before restitution sees it.
					MinimumRetainedAmount = BaselineAmount;
				}
			}
			if (SourceCell->Amount <= MinimumRetainedAmount)
			{
				// Restitution is part of the active body/material transaction. A
				// recently vacated high-bed column may receive conserved particles,
				// but cannot immediately donate them again in the same transaction.
				return false;
			}
			const int8 AxisBias = PuddleShape
				? PuddleShape->AxisBias
				: 0;
			const auto RadialDistance = [PuddleShape](const FIntPoint& Cell)
			{
				if (!PuddleShape)
				{
					return 0.0f;
				}
				const FVector2D Delta(
					static_cast<double>(Cell.X) - PuddleShape->Center.X,
					static_cast<double>(Cell.Y) - PuddleShape->Center.Y);
				return static_cast<float>(Delta.Length());
			};

			// 小型水洼以液量推导目标圆盘。超出圆盘的低量边缘优先并回
			// 更靠近质心的同材质格，这一步只做局部转移并严格守恒。
			if (PuddleShape)
			{
				const float SourceRadius = RadialDistance(Source);
				if (SourceRadius > PuddleShape->Radius + 0.35f
					&& SourceCell->Amount <= 176)
				{
					FIntPoint MergeDestination = Source;
					int32 BestCapacity = 0;
					float BestRadius = SourceRadius;
					for (const FIntPoint Direction : Directions)
					{
						FIntPoint Neighbor;
						if (!TryOffsetCell(Source, Direction, Neighbor))
						{
							continue;
						}
						const FCell* NeighborCell = FindCell(Neighbor);
						if (!NeighborCell
							|| NeighborCell->MaterialIndex
								!= SourceCell->MaterialIndex)
						{
							continue;
						}
						const float NeighborRadius = RadialDistance(Neighbor);
						const int32 Capacity = FullCellAmount
							- static_cast<int32>(NeighborCell->Amount);
						if (Capacity > 0
							&& NeighborRadius + 0.01f < BestRadius
							&& (Capacity > BestCapacity
								|| (Capacity == BestCapacity
									&& NeighborRadius < BestRadius)))
						{
							MergeDestination = Neighbor;
							BestCapacity = Capacity;
							BestRadius = NeighborRadius;
						}
					}
					if (MergeDestination != Source)
					{
						FCell& MutableSourceCell = FindOrAddCell(Source);
						FCell& DestinationCell = FindOrAddCell(MergeDestination);
						const int32 TransferAmount = FMath::Min(
							static_cast<int32>(MutableSourceCell.Amount)
								- MinimumRetainedAmount,
							BestCapacity);
						DestinationCell.Amount = static_cast<uint8>(
							static_cast<int32>(DestinationCell.Amount)
								+ TransferAmount);
						if (DestinationCell.RemainingLifetime != 0
							&& MutableSourceCell.RemainingLifetime != 0)
						{
							DestinationCell.RemainingLifetime = FMath::Min(
								DestinationCell.RemainingLifetime,
								MutableSourceCell.RemainingLifetime);
						}
						MutableSourceCell.Amount = static_cast<uint8>(
							static_cast<int32>(MutableSourceCell.Amount)
							- TransferAmount);
						if (MutableSourceCell.Amount == 0)
						{
							MutableSourceCell.MaterialIndex = 0;
							MutableSourceCell.RemainingLifetime = 0;
						}
						DestinationCell.LastUpdatedTick = Tick;
						MutableSourceCell.LastUpdatedTick = Tick;
						MarkDirtyNeighborhood(Source);
						MarkDirtyNeighborhood(MergeDestination);
						++Stats.MovedCells;
						return true;
					}
				}
			}

			// 表面张力会把只靠少量邻格连接的极薄毛刺并回水洼主体。
			// 液量被完整转移而非删除，避免随机边缘退化成孤立方块。
			if (MinimumRetainedAmount == 0
				&& SourceCell->Amount <= MinimumLiquidEdgeAmount + 4)
			{
				int32 SameMaterialNeighborCount = 0;
				uint8 BestNeighborAmount = 0;
				FIntPoint BestNeighbor = Source;
				for (const FIntPoint Direction : Directions)
				{
					FIntPoint Neighbor;
					if (!TryOffsetCell(Source, Direction, Neighbor))
					{
						continue;
					}
					const FCell* NeighborCell = FindCell(Neighbor);
					if (!NeighborCell
						|| NeighborCell->MaterialIndex
							!= SourceCell->MaterialIndex)
					{
						continue;
					}
					++SameMaterialNeighborCount;
					if (NeighborCell->Amount >= BestNeighborAmount
						&& static_cast<int32>(NeighborCell->Amount)
							+ static_cast<int32>(SourceCell->Amount)
							<= FullCellAmount)
					{
						BestNeighborAmount = NeighborCell->Amount;
						BestNeighbor = Neighbor;
					}
				}
				if (SameMaterialNeighborCount <= 3
					&& BestNeighbor != Source)
				{
					FCell& NeighborCell = FindOrAddCell(BestNeighbor);
					FCell& MutableSourceCell = FindOrAddCell(Source);
					NeighborCell.Amount = static_cast<uint8>(
						static_cast<int32>(NeighborCell.Amount)
						+ static_cast<int32>(MutableSourceCell.Amount));
					if (NeighborCell.RemainingLifetime != 0
						&& MutableSourceCell.RemainingLifetime != 0)
					{
						NeighborCell.RemainingLifetime = FMath::Min(
							NeighborCell.RemainingLifetime,
							MutableSourceCell.RemainingLifetime);
					}
					MutableSourceCell.MaterialIndex = 0;
					MutableSourceCell.Amount = 0;
					MutableSourceCell.RemainingLifetime = 0;
					NeighborCell.LastUpdatedTick = Tick;
					MutableSourceCell.LastUpdatedTick = Tick;
					MarkDirtyNeighborhood(Source);
					MarkDirtyNeighborhood(BestNeighbor);
					++Stats.MovedCells;
					return true;
				}
			}
			bool bFoundLateralDestination = false;
			bool bLowestInteriorLiquidCoordinate = false;
			bool bLowestRecentBodyVacancy = false;
			uint8 LowestAmount = FullCellAmount;
			int64 LowestEffectiveSurface255 = MAX_int64;
			int64 LowestSurface255 = MAX_int64;
			FIntPoint LateralDestination = Source;
			const int64 SourceSurface255 =
				static_cast<int64>(SourceSupport) * FullCellAmount
				+ static_cast<int64>(Settings.LiquidFullColumnHeight)
					* SourceCell->Amount;
			for (int32 Offset = 0;
				Offset < UE_ARRAY_COUNT(Directions);
				++Offset)
			{
				const int32 DirectionIndex =
					(FirstDirection + Offset)
						% UE_ARRAY_COUNT(Directions);
				FIntPoint Destination;
				if (!TryOffsetCell(
						Source,
						Directions[DirectionIndex],
						Destination)
					|| (Settings.bCullOutsideSurfaceBounds
						&& !IsInsideSurfaceBounds(Destination)))
				{
					continue;
				}
				int16 DestinationSupport = 0;
				if (!FindSupportHeight(Destination, DestinationSupport))
				{
					continue;
				}
				const FCell* DestinationCell = FindCell(Destination);
				if (DestinationCell
					&& DestinationCell->MaterialIndex != 0
					&& DestinationCell->MaterialIndex
						!= SourceCell->MaterialIndex)
				{
					continue;
				}
				int32 CardinalLiquidNeighborCount = 0;
				for (int32 NeighborIndex = 0; NeighborIndex < 4; ++NeighborIndex)
				{
					FIntPoint Neighbor;
					if (!TryOffsetCell(
							Destination,
							Directions[NeighborIndex],
							Neighbor))
					{
						continue;
					}
					const FCell* NeighborCell = FindCell(Neighbor);
					CardinalLiquidNeighborCount += NeighborCell
						&& NeighborCell->MaterialIndex
							== SourceCell->MaterialIndex
						&& NeighborCell->Amount > 0 ? 1 : 0;
				}
				const bool bRecentBodyVacancy =
					BodyDisplacementVacancies.Contains(Destination)
					&& DestinationCell
					&& DestinationCell->bHasBodyDisplacementVacancy
					&& DestinationCell->BodyDisplacedMaterialIndex
						== SourceCell->MaterialIndex;
				const bool bInteriorLiquidCoordinate =
					CardinalLiquidNeighborCount >= 2 || bRecentBodyVacancy;
				// Uneven terrain does not disconnect a liquid. Permit cross-support
				// flow where the current particles surround the coordinate (an interior
				// vacancy/wake), while a single-neighbour shoreline still obeys its
				// authored bank and surface tension.
				if (DestinationSupport != SourceSupport
					&& DestinationSupport >= SourceSupport
					&& !bInteriorLiquidCoordinate)
				{
					continue;
				}
				const uint8 DestinationAmount =
					DestinationCell ? DestinationCell->Amount : 0;
				if (DestinationAmount >= FullCellAmount)
				{
					continue;
				}
				const int64 DestinationSurface255 =
					static_cast<int64>(DestinationSupport) * FullCellAmount
					+ static_cast<int64>(Settings.LiquidFullColumnHeight)
						* DestinationAmount;
				if (DestinationSurface255 >= SourceSurface255)
				{
					continue;
				}
				int32 DirectionPenalty = 0;
				// A lower dry shoreline can win the candidate search and then fail its
				// edge-tension gate, starving an equally reachable body wake forever.
				// Prefer current interior/recently displaced coordinates during choice;
				// the later transfer still uses unmodified physical surface heights.
				if (bRecentBodyVacancy)
				{
					DirectionPenalty -= 512;
				}
				else if (bInteriorLiquidCoordinate)
				{
					DirectionPenalty -= 128;
				}
				if (DestinationAmount == 0)
				{
					const FIntPoint Direction = Directions[DirectionIndex];
					if (PuddleShape)
					{
						const float DestinationRadius =
							RadialDistance(Destination);
						const uint32 BoundaryHash = MixBits(
							Seed
								^ static_cast<uint32>(Destination.X) * 0x27d4eb2du
								^ static_cast<uint32>(Destination.Y) * 0x165667b1u);
						const float BoundaryJitter =
							(static_cast<float>(BoundaryHash % 17u) - 8.0f)
							* 0.045f;
						const float EffectiveRadius =
							PuddleShape->Radius + BoundaryJitter;
						if (DestinationRadius > EffectiveRadius)
						{
							DirectionPenalty += 112
								+ FMath::RoundToInt(
									(DestinationRadius - EffectiveRadius)
									* 24.0f);
						}
						else if (DestinationRadius + 0.1f
							< RadialDistance(Source))
						{
							DirectionPenalty -= 24;
						}
					}
					if (AxisBias != 0)
					{
						const bool bAlongLongAxis = AxisBias < 0
							? Direction.X == 0
							: Direction.Y == 0;
						const bool bDiagonal =
							Direction.X != 0 && Direction.Y != 0;
						constexpr int32 AxisPenalty = 48;
						DirectionPenalty = bAlongLongAxis
							? AxisPenalty
							: (bDiagonal ? AxisPenalty / 2 : 0);
					}
				}
				const int64 EffectiveSurface255 =
					DestinationSurface255
					+ static_cast<int64>(DirectionPenalty)
						* Settings.LiquidFullColumnHeight;
				if (!bFoundLateralDestination
					|| EffectiveSurface255 < LowestEffectiveSurface255)
				{
					bFoundLateralDestination = true;
					LowestAmount = DestinationAmount;
					LowestEffectiveSurface255 = EffectiveSurface255;
					LowestSurface255 = DestinationSurface255;
					LateralDestination = Destination;
					bLowestInteriorLiquidCoordinate =
						bInteriorLiquidCoordinate;
					bLowestRecentBodyVacancy = bRecentBodyVacancy;
				}
			}
			if (!bFoundLateralDestination)
			{
				return false;
			}

			const uint32 EdgeHash = MixBits(
				Seed
				^ static_cast<uint32>(LateralDestination.X) * 0x27d4eb2du
				^ static_cast<uint32>(LateralDestination.Y) * 0x165667b1u
				^ static_cast<uint32>(SourceCell->MaterialIndex) * 0x85ebca6bu);
			int32 SurfaceTension = LowestAmount == 0
				? (bLowestInteriorLiquidCoordinate
					? 8
					: 84 + static_cast<int32>(EdgeHash % 37u))
				: 8;
			if (LowestAmount == 0 && AxisBias != 0)
			{
				const FIntPoint Movement = LateralDestination - Source;
				const bool bAlongLongAxis = AxisBias < 0
					? Movement.X == 0
					: Movement.Y == 0;
				const bool bAlongShortAxis = AxisBias < 0
					? Movement.Y == 0
					: Movement.X == 0;
				SurfaceTension = bAlongLongAxis
					? SurfaceTension + 32
					: (bAlongShortAxis
						? FMath::Max(SurfaceTension - 32, 24)
						: SurfaceTension);
			}
			if (LowestAmount == 0 && PuddleShape)
			{
				const float DestinationRadius =
					RadialDistance(LateralDestination);
				SurfaceTension = DestinationRadius <= PuddleShape->Radius
					? FMath::Max(SurfaceTension - 28, 24)
					: SurfaceTension + 56;
			}
			const int32 AmountDifference = static_cast<int32>(
				(SourceSurface255 - LowestSurface255)
					/ Settings.LiquidFullColumnHeight);
			if (AmountDifference <= SurfaceTension + 1)
			{
				return false;
			}
			int32 TransferAmount = FMath::Min(
				(AmountDifference - SurfaceTension) / 2,
				MaximumLiquidTransferPerStep);
			if (LowestAmount == 0
				&& TransferAmount < MinimumLiquidEdgeAmount)
			{
				return false;
			}

			const uint16 SourceMaterialIndex = SourceCell->MaterialIndex;
			WakeSurfaceFlow(LateralDestination);
			FCell& DestinationCell = FindOrAddCell(LateralDestination);
			FCell& MutableSourceCell = FindOrAddCell(Source);
			int32 TransferMinimumRetainedAmount = MinimumRetainedAmount;
			if (bLowestRecentBodyVacancy)
			{
				uint16 BaselineMaterialIndex = 0;
				int16 BaselineSupportHeight = 0;
				uint16 BaselineAmount = 0;
				if (FindBaselineCell(
						Source,
						BaselineMaterialIndex,
						BaselineSupportHeight,
						BaselineAmount)
					&& BaselineMaterialIndex == SourceMaterialIndex)
				{
					// An active body wake may accept displaced surplus through the
					// ordinary local-pressure path, but it must not drain the canonical
					// particles of a neighboring lake column and relocate the hole.
					TransferMinimumRetainedAmount = FMath::Max(
						TransferMinimumRetainedAmount,
						static_cast<int32>(BaselineAmount));
				}
			}
			TransferAmount = FMath::Min(
				TransferAmount,
				FMath::Min(
					static_cast<int32>(MutableSourceCell.Amount)
						- TransferMinimumRetainedAmount,
					FullCellAmount
						- static_cast<int32>(DestinationCell.Amount)));
			if (TransferAmount <= 0)
			{
				return false;
			}
			const bool bDestinationWasEmpty = DestinationCell.MaterialIndex == 0;
			DestinationCell.MaterialIndex = SourceMaterialIndex;
			if (bDestinationWasEmpty)
			{
				DestinationCell.RemainingLifetime =
					MutableSourceCell.RemainingLifetime;
			}
			else if (DestinationCell.RemainingLifetime != 0
				&& MutableSourceCell.RemainingLifetime != 0)
			{
				DestinationCell.RemainingLifetime = FMath::Min(
					DestinationCell.RemainingLifetime,
					MutableSourceCell.RemainingLifetime);
			}
			DestinationCell.Amount = static_cast<uint8>(
				FMath::Min(
					static_cast<int32>(FullCellAmount),
					static_cast<int32>(DestinationCell.Amount)
						+ TransferAmount));
			MutableSourceCell.Amount = static_cast<uint8>(
				static_cast<int32>(MutableSourceCell.Amount)
				- TransferAmount);
			if (MutableSourceCell.Amount == 0)
			{
				MutableSourceCell.MaterialIndex = 0;
				MutableSourceCell.RemainingLifetime = 0;
			}
			DestinationCell.LastUpdatedTick = Tick;
			MutableSourceCell.LastUpdatedTick = Tick;
			MarkDirtyNeighborhood(Source);
			MarkDirtyNeighborhood(LateralDestination);
			++Stats.MovedCells;
			return true;
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
				SourceCell.Amount = 0;
				SourceCell.RemainingLifetime = 0;
				SourceCell.LastUpdatedTick = Tick;
				MarkDirtyNeighborhood(Source);
				++Stats.CulledCells;
				return true;
			}

			if (!IsChunkActive(ToChunkCoordinate(Destination)))
			{
				if (!Settings.bUseSurfaceTopology)
				{
					return false;
				}
				WakeSurfaceFlow(Destination);
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
				Swap(SourceCell.Amount, DestinationCell.Amount);
				Swap(SourceCell.RemainingLifetime, DestinationCell.RemainingLifetime);
				SourceCell.LastUpdatedTick = Tick;
				DestinationCell.LastUpdatedTick = Tick;
			}
			else
			{
				DestinationCell.MaterialIndex =
					SourceCell.MaterialIndex;
				DestinationCell.Amount = SourceCell.Amount;
				DestinationCell.RemainingLifetime = SourceCell.RemainingLifetime;
				DestinationCell.LastUpdatedTick = Tick;
				SourceCell.MaterialIndex = 0;
				SourceCell.Amount = 0;
				SourceCell.RemainingLifetime = 0;
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
				MatterFlux::Reaction::FDeterministicContext Context;
				Context.Seed = static_cast<int32>(Seed);
				Context.Tick = Tick;
				Context.FirstCell = FirstPosition;
				Context.SecondCell = SecondPosition;
				MatterFlux::Reaction::FContactResult ContactResult;
				if (!MatterFlux::Reaction::FMaterialReactionEngine::EvaluateContact(
						Reaction.Rule,
						Materials[FirstCell->MaterialIndex].Id,
						Materials[SecondCell->MaterialIndex].Id,
						Context,
						ContactResult)
					|| !ContactResult.bReacted)
				{
					continue;
				}
				const auto ResolveResult = [this](const FName MaterialId)
				{
					if (MaterialId.IsNone() || MaterialId == TEXT("empty"))
					{
						return static_cast<uint16>(0);
					}
					return MaterialIndices.FindChecked(MaterialId);
				};
				FirstCell->MaterialIndex = ResolveResult(
					ContactResult.FirstMaterial);
				SecondCell->MaterialIndex = ResolveResult(
					ContactResult.SecondMaterial);
				FirstCell->RemainingLifetime = FirstCell->MaterialIndex == 0
					? 0
					: Materials[FirstCell->MaterialIndex].LifetimeSteps;
				SecondCell->RemainingLifetime = SecondCell->MaterialIndex == 0
					? 0
					: Materials[SecondCell->MaterialIndex].LifetimeSteps;
				if (FirstCell->MaterialIndex == 0)
				{
					FirstCell->Amount = 0;
				}
				if (SecondCell->MaterialIndex == 0)
				{
					SecondCell->Amount = 0;
				}
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
			&& LiquidFullColumnHeight > 0
			&& LiquidFullColumnHeight <= MAX_int16
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
				|| Impl->Materials.Num() > MaterialIndexMask)
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
			Material.LifetimeSteps = Definition.LifetimeSteps;
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
			if (Definition.Kind != FMatterFluxReactionDefinition::EKind::Contact)
			{
				continue;
			}
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

			uint16 ResolvedOutputA = 0;
			uint16 ResolvedOutputB = 0;
			FImpl::FResolvedReaction Reaction;
			if (ReactionId.IsNone()
				|| !InputA
				|| !InputB
				|| !ResolveOutput(Definition.OutputA, ResolvedOutputA)
				|| !ResolveOutput(Definition.OutputB, ResolvedOutputB)
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
			Reaction.Rule = Definition;
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
		Cell.Amount = MaterialIndex == 0 ? 0 : FullCellAmount;
		Cell.RemainingLifetime = MaterialIndex == 0
			? 0
			: Impl->Materials[MaterialIndex].LifetimeSteps;
		Cell.LastUpdatedTick = 0;
		Cell.BodyDisplacedTick = 0;
		Cell.BodyDisplacedMaterialIndex = 0;
		Cell.BodyDisplacedReferenceAmount = 0;
		Cell.bHasBodyDisplacementVacancy = false;
		Impl->BodyDisplacementVacancies.Remove(WorldCell);
		Impl->MarkDirtyNeighborhood(WorldCell);
		const FIntPoint ChunkCoordinate =
			Impl->ToChunkCoordinate(WorldCell);
		if (!Impl->IsChunkActive(ChunkCoordinate))
		{
			Impl->ArchiveChunk(ChunkCoordinate);
		}
		return true;
	}

	bool FChunkedMaterialWorld::SetCellAmount(
		const FIntPoint& WorldCell,
		const FName MaterialId,
		const uint16 Amount)
	{
		if (!Impl->bInitialized
			|| MaterialId.IsNone()
			|| Amount == 0
			|| (Impl->Settings.bUseSurfaceTopology
				? Impl->Settings.bCullOutsideSurfaceBounds
					&& !Impl->IsInsideSurfaceBounds(WorldCell)
				: Impl->Settings.bCullOutsideVerticalBounds
					&& !Impl->IsInsideVerticalBounds(WorldCell.Y)))
		{
			return false;
		}

		const uint16* MaterialIndex =
			Impl->MaterialIndices.Find(MaterialId);
		if (!MaterialIndex)
		{
			return false;
		}

		FImpl::FCell& Cell = Impl->FindOrAddCell(WorldCell);
		Cell.MaterialIndex = *MaterialIndex;
		Cell.Amount = Amount;
		Cell.RemainingLifetime =
			Impl->Materials[*MaterialIndex].LifetimeSteps;
		Cell.LastUpdatedTick = 0;
		Cell.BodyDisplacedTick = 0;
		Cell.BodyDisplacedMaterialIndex = 0;
		Cell.BodyDisplacedReferenceAmount = 0;
		Cell.bHasBodyDisplacementVacancy = false;
		Impl->BodyDisplacementVacancies.Remove(WorldCell);
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
			Cell.Amount = Cell.MaterialIndex == 0
				? 0
				: SeedCell.Amount;
			Cell.RemainingLifetime = Cell.MaterialIndex == 0
				? 0
				: Impl->Materials[Cell.MaterialIndex].LifetimeSteps;
			Cell.LastUpdatedTick = 0;
			Cell.BodyDisplacedTick = 0;
			Cell.BodyDisplacedMaterialIndex = 0;
			Cell.BodyDisplacedReferenceAmount = 0;
			Cell.bHasBodyDisplacementVacancy = false;
			Impl->BodyDisplacementVacancies.Remove(SeedCell.WorldCell);
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
		// Streaming adds one surface chunk at a time. Preserve every untouched
		// baseline fact and refresh only this transaction's chunks instead of
		// copying and serializing the entire explored material world after every
		// addition. The initial full-map seed still touches every chunk and thus
		// produces the same complete baseline.
		for (const FIntPoint& ChunkCoordinate : TouchedChunks)
		{
			if (const FImpl::FArchivedChunk* Archive =
				Impl->ArchivedChunks.Find(ChunkCoordinate))
			{
				Impl->BaselineChunks.Add(ChunkCoordinate, *Archive);
			}
			else if (const TUniquePtr<FImpl::FChunk>* Chunk =
				Impl->Chunks.Find(ChunkCoordinate);
				Chunk && Chunk->IsValid())
			{
				Impl->BaselineChunks.Add(
					ChunkCoordinate,
					Impl->EncodeChunk(**Chunk));
			}
			else
			{
				Impl->BaselineChunks.Remove(ChunkCoordinate);
			}
			if (Impl->IsChunkActive(ChunkCoordinate))
			{
				Impl->MarkChunkForBaselineResume(ChunkCoordinate);
			}
		}
		return true;
	}

	int32 FChunkedMaterialWorld::DisplaceLiquids(
		const TConstArrayView<FIntPoint> OccupiedCells,
		const int32 MaxSearchRadius)
	{
		TArray<FLiquidDisplacementConstraint> Constraints;
		Constraints.Reserve(OccupiedCells.Num());
		for (const FIntPoint Cell : OccupiedCells)
		{
			Constraints.Add({ Cell, 0 });
		}
		return DisplaceLiquids(Constraints, MaxSearchRadius);
	}

	int32 FChunkedMaterialWorld::DisplaceLiquids(
		const TConstArrayView<FLiquidDisplacementConstraint> Constraints,
		const int32 MaxSearchRadius)
	{
		Impl->LastLiquidDisplacementStats = {};
		if (!Impl->bInitialized
			|| Constraints.IsEmpty()
			|| MaxSearchRadius <= 0)
		{
			return 0;
		}

		TMap<FIntPoint, uint8> MaximumRemainingAmounts;
		MaximumRemainingAmounts.Reserve(Constraints.Num());
		for (const FLiquidDisplacementConstraint& Constraint : Constraints)
		{
			uint8* Existing = MaximumRemainingAmounts.Find(
				Constraint.WorldCell);
			if (Existing)
			{
				*Existing = FMath::Min(
					*Existing, Constraint.MaximumRemainingAmount);
			}
			else
			{
				MaximumRemainingAmounts.Add(
					Constraint.WorldCell,
					Constraint.MaximumRemainingAmount);
			}
		}

		TArray<FIntPoint> StableOccupiedCells;
		MaximumRemainingAmounts.GetKeys(StableOccupiedCells);
		StableOccupiedCells.Sort([](const FIntPoint A, const FIntPoint B)
		{
			return A.Y != B.Y ? A.Y < B.Y : A.X < B.X;
		});

		// Constraints from every buoyancy component arrive in one sparse map.
		// Split them into connected body footprints so nearby columns share one
		// pressure solve while separated actors cannot transfer liquid between
		// unrelated lakes.
		TSet<FIntPoint> UnvisitedOccupiedCells;
		UnvisitedOccupiedCells.Reserve(StableOccupiedCells.Num());
		for (const FIntPoint Cell : StableOccupiedCells)
		{
			UnvisitedOccupiedCells.Add(Cell);
		}
		TArray<TArray<FIntPoint>> ConnectedFootprints;
		for (const FIntPoint StartCell : StableOccupiedCells)
		{
			if (UnvisitedOccupiedCells.Remove(StartCell) == 0)
			{
				continue;
			}
			TArray<FIntPoint>& Footprint = ConnectedFootprints.AddDefaulted_GetRef();
			TArray<FIntPoint> Queue;
			Queue.Add(StartCell);
			for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
			{
				const FIntPoint Cell = Queue[QueueIndex];
				Footprint.Add(Cell);
				for (int32 Y = -1; Y <= 1; ++Y)
				{
					for (int32 X = -1; X <= 1; ++X)
					{
						if (X == 0 && Y == 0)
						{
							continue;
						}
						FIntPoint Neighbor;
						if (TryOffsetCell(Cell, FIntPoint(X, Y), Neighbor)
							&& UnvisitedOccupiedCells.Remove(Neighbor) > 0)
						{
							Queue.Add(Neighbor);
						}
					}
				}
			}
			Footprint.Sort([](const FIntPoint A, const FIntPoint B)
			{
				return A.Y != B.Y ? A.Y < B.Y : A.X < B.X;
			});
		}

		struct FSourceState
		{
			FIntPoint WorldCell = FIntPoint::ZeroValue;
			int32 AmountToMove = 0;
			uint8 BodyDisplacedReferenceAmount = 0;
			uint8 RemainingLifetime = 0;
		};
		struct FPressureDestination
		{
			FIntPoint WorldCell = FIntPoint::ZeroValue;
			int64 SurfaceHeight255 = 0;
			uint32 StableOrder = 0;
			uint8 Amount = 0;
			int32 TransferAmount = 0;
		};
		struct FEmptyDestination
		{
			FIntPoint WorldCell = FIntPoint::ZeroValue;
			int32 DistanceToFootprintBounds = 0;
			uint32 StableOrder = 0;
			int32 TransferAmount = 0;
		};

		int32 MovedCells = 0;
		TSet<FIntPoint> TouchedChunks;
		const int32 SearchRadius = FMath::Clamp(MaxSearchRadius, 1, 256);
		for (const TArray<FIntPoint>& Footprint : ConnectedFootprints)
		{
			++Impl->LastLiquidDisplacementStats.ConnectedFootprints;
			TMap<uint16, TArray<FSourceState>> SourcesByMaterial;
			for (const FIntPoint Source : Footprint)
			{
				FImpl::FCell* SourceCell = Impl->FindCell(Source);
				if (!SourceCell)
				{
					const uint16 ArchivedMaterialIndex =
						Impl->FindArchivedMaterialIndex(Source);
					if (ArchivedMaterialIndex != 0
						&& ArchivedMaterialIndex < Impl->Materials.Num()
						&& Impl->Materials[ArchivedMaterialIndex].Phase
							== EMatterFluxMaterialPhase::Liquid)
					{
						SourceCell = &Impl->FindOrAddCell(Source);
						TouchedChunks.Add(Impl->ToChunkCoordinate(Source));
					}
				}
				if (!SourceCell
					|| SourceCell->MaterialIndex == 0
					|| SourceCell->Amount == 0
					|| SourceCell->MaterialIndex >= Impl->Materials.Num()
					|| Impl->Materials[SourceCell->MaterialIndex].Phase
						!= EMatterFluxMaterialPhase::Liquid)
				{
					continue;
				}

				const int32 AmountToMove = FMath::Max(
					static_cast<int32>(SourceCell->Amount)
						- MaximumRemainingAmounts.FindChecked(Source),
					0);
				if (AmountToMove <= 0)
				{
					continue;
				}

				uint8 ReferenceAmount =
					SourceCell->bHasBodyDisplacementVacancy
						&& SourceCell->BodyDisplacedReferenceAmount > 0
						? SourceCell->BodyDisplacedReferenceAmount
						: SourceCell->Amount;
				uint16 BaselineMaterialIndex = 0;
				int16 BaselineSupportHeight = 0;
				uint16 BaselineAmount = 0;
				if (Impl->FindBaselineCell(
						Source,
						BaselineMaterialIndex,
						BaselineSupportHeight,
						BaselineAmount)
					&& BaselineMaterialIndex == SourceCell->MaterialIndex
					&& BaselineAmount > 0)
				{
					ReferenceAmount = BaselineAmount;
				}

				SourcesByMaterial.FindOrAdd(SourceCell->MaterialIndex).Add({
					Source,
					AmountToMove,
					ReferenceAmount,
					SourceCell->RemainingLifetime });
				++Impl->LastLiquidDisplacementStats.SourceCells;
			}

			TArray<uint16> StableMaterialIndices;
			SourcesByMaterial.GetKeys(StableMaterialIndices);
			StableMaterialIndices.Sort();
			for (const uint16 MaterialIndex : StableMaterialIndices)
			{
				TArray<FSourceState>& Sources =
					SourcesByMaterial.FindChecked(MaterialIndex);
				Sources.Sort([](const FSourceState& A, const FSourceState& B)
				{
					return A.WorldCell.Y != B.WorldCell.Y
						? A.WorldCell.Y < B.WorldCell.Y
						: A.WorldCell.X < B.WorldCell.X;
				});

				int64 RemainingToMove = 0;
				uint8 TransferLifetime = 0;
				int32 SourceMinX = MAX_int32;
				int32 SourceMaxX = MIN_int32;
				int32 SourceMinY = MAX_int32;
				int32 SourceMaxY = MIN_int32;
				for (const FSourceState& Source : Sources)
				{
					RemainingToMove += Source.AmountToMove;
					if (Source.RemainingLifetime != 0)
					{
						TransferLifetime = TransferLifetime == 0
							? Source.RemainingLifetime
							: FMath::Min(
								TransferLifetime,
								Source.RemainingLifetime);
					}
					SourceMinX = FMath::Min(SourceMinX, Source.WorldCell.X);
					SourceMaxX = FMath::Max(SourceMaxX, Source.WorldCell.X);
					SourceMinY = FMath::Min(SourceMinY, Source.WorldCell.Y);
					SourceMaxY = FMath::Max(SourceMaxY, Source.WorldCell.Y);
				}
				const int64 InitialAmountToMove = RemainingToMove;
				if (RemainingToMove <= 0)
				{
					continue;
				}

				// Build the union of all radius squares as merged row intervals.
				// For N adjacent source cells this costs N*(2R+1) interval inserts
				// plus one visit per unique candidate, instead of N*(2R+1)^2.
				TMap<int32, TArray<FIntPoint>> RowIntervals;
				for (const FSourceState& Source : Sources)
				{
					for (int32 YOffset = -SearchRadius;
						YOffset <= SearchRadius;
						++YOffset)
					{
						FIntPoint Left;
						FIntPoint Right;
						if (TryOffsetCell(
								Source.WorldCell,
								FIntPoint(-SearchRadius, YOffset),
								Left)
							&& TryOffsetCell(
								Source.WorldCell,
								FIntPoint(SearchRadius, YOffset),
								Right))
						{
							RowIntervals.FindOrAdd(Left.Y).Add(
								FIntPoint(Left.X, Right.X));
						}
					}
				}

				TArray<FPressureDestination> PressureDestinations;
				TArray<FEmptyDestination> EmptyDestinations;
				TArray<int32> StableRows;
				RowIntervals.GetKeys(StableRows);
				StableRows.Sort();
				const uint32 FootprintHash = GetTypeHash(Footprint[0]);
				for (const int32 Row : StableRows)
				{
					TArray<FIntPoint>& Intervals = RowIntervals.FindChecked(Row);
					Intervals.Sort([](const FIntPoint A, const FIntPoint B)
					{
						return A.X != B.X ? A.X < B.X : A.Y < B.Y;
					});
					int32 IntervalStart = Intervals[0].X;
					int32 IntervalEnd = Intervals[0].Y;
					const auto VisitInterval =
						[&](const int32 StartX, const int32 EndX)
						{
							for (int64 CandidateX = StartX;
								CandidateX <= static_cast<int64>(EndX);
								++CandidateX)
							{
								++Impl->LastLiquidDisplacementStats
									.CandidateCellsVisited;
								const FIntPoint Candidate(
									static_cast<int32>(CandidateX), Row);
								if (MaximumRemainingAmounts.Contains(Candidate)
									|| (Impl->Settings.bUseSurfaceTopology
										? Impl->Settings.bCullOutsideSurfaceBounds
											&& !Impl->IsInsideSurfaceBounds(Candidate)
										: Impl->Settings.bCullOutsideVerticalBounds
											&& !Impl->IsInsideVerticalBounds(Candidate.Y)))
								{
									continue;
								}
								int16 SupportHeight = 0;
								if (Impl->Settings.bUseSurfaceTopology
									&& !Impl->FindSupportHeight(Candidate, SupportHeight))
								{
									continue;
								}
								const FImpl::FCell* CandidateCell =
									Impl->FindCell(Candidate);
								const uint16 CandidateMaterialIndex = CandidateCell
									? CandidateCell->MaterialIndex
									: Impl->FindArchivedMaterialIndex(Candidate);
								const uint8 CandidateAmount = CandidateCell
									? CandidateCell->Amount
									: Impl->FindArchivedAmount(Candidate);
								const uint32 StableOrder = HashCombine(
									FootprintHash, GetTypeHash(Candidate));
								if (CandidateMaterialIndex == MaterialIndex
									&& CandidateAmount < FullCellAmount)
								{
									PressureDestinations.Add({
										Candidate,
										static_cast<int64>(SupportHeight)
											* FullCellAmount
											+ static_cast<int64>(
												Impl->Settings.LiquidFullColumnHeight)
												* CandidateAmount,
										StableOrder,
										CandidateAmount,
										0 });
								}
								else if (CandidateMaterialIndex == 0)
								{
									const int32 DistanceX = FMath::Max3(
										SourceMinX - Candidate.X,
										0,
										Candidate.X - SourceMaxX);
									const int32 DistanceY = FMath::Max3(
										SourceMinY - Candidate.Y,
										0,
										Candidate.Y - SourceMaxY);
									EmptyDestinations.Add({
										Candidate,
										FMath::Max(DistanceX, DistanceY),
										StableOrder,
										0 });
								}
							}
						};
					for (int32 IntervalIndex = 1;
						IntervalIndex < Intervals.Num();
						++IntervalIndex)
					{
						const FIntPoint Interval = Intervals[IntervalIndex];
						if (static_cast<int64>(Interval.X)
							<= static_cast<int64>(IntervalEnd) + 1)
						{
							IntervalEnd = FMath::Max(IntervalEnd, Interval.Y);
						}
						else
						{
							VisitInterval(IntervalStart, IntervalEnd);
							IntervalStart = Interval.X;
							IntervalEnd = Interval.Y;
						}
					}
					VisitInterval(IntervalStart, IntervalEnd);
				}

				// Each destination contributes a sorted arithmetic sequence of
				// possible quantum placements. Select the first K placements by one
				// height cutoff, then resolve the equal-height remainder in the same
				// deterministic order. This is equivalent to unit heap updates without
				// one pop/push per liquid particle.
				int64 PressureCapacity = 0;
				int64 LowestEventHeight = MAX_int64;
				int64 HighestEventHeight = MIN_int64;
				const int64 HeightStep = FMath::Max(
					Impl->Settings.LiquidFullColumnHeight, 1);
				for (const FPressureDestination& Destination
					: PressureDestinations)
				{
					const int32 Capacity =
						FullCellAmount - Destination.Amount;
					PressureCapacity += Capacity;
					if (Capacity > 0)
					{
						LowestEventHeight = FMath::Min(
							LowestEventHeight, Destination.SurfaceHeight255);
						HighestEventHeight = FMath::Max(
							HighestEventHeight,
							Destination.SurfaceHeight255
								+ static_cast<int64>(Capacity - 1) * HeightStep);
					}
				}
				const int64 PressureTransfer = FMath::Min(
					RemainingToMove, PressureCapacity);
				if (PressureTransfer > 0)
				{
					int64 Low = LowestEventHeight;
					int64 High = HighestEventHeight;
					while (Low < High)
					{
						const int64 Middle = Low + (High - Low) / 2;
						int64 PlacementCount = 0;
						for (const FPressureDestination& Destination
							: PressureDestinations)
						{
							if (Middle < Destination.SurfaceHeight255)
							{
								continue;
							}
							const int64 Capacity =
								FullCellAmount - Destination.Amount;
							PlacementCount += FMath::Min(
								Capacity,
								(Middle - Destination.SurfaceHeight255)
									/ HeightStep + 1);
							if (PlacementCount >= PressureTransfer)
							{
								break;
							}
						}
						if (PlacementCount >= PressureTransfer)
						{
							High = Middle;
						}
						else
						{
							Low = Middle + 1;
						}
					}
					const int64 CutoffHeight = Low;
					int64 PlacementsBelowCutoff = 0;
					TArray<int32> CutoffTies;
					for (int32 DestinationIndex = 0;
						DestinationIndex < PressureDestinations.Num();
						++DestinationIndex)
					{
						FPressureDestination& Destination =
							PressureDestinations[DestinationIndex];
						const int64 Capacity =
							FullCellAmount - Destination.Amount;
						if (CutoffHeight > Destination.SurfaceHeight255)
						{
							Destination.TransferAmount = static_cast<int32>(
								FMath::Min(
									Capacity,
									(CutoffHeight - 1
										- Destination.SurfaceHeight255)
										/ HeightStep + 1));
						}
						PlacementsBelowCutoff += Destination.TransferAmount;
						if (Destination.TransferAmount < Capacity
							&& Destination.SurfaceHeight255
								+ static_cast<int64>(Destination.TransferAmount)
									* HeightStep == CutoffHeight)
						{
							CutoffTies.Add(DestinationIndex);
						}
					}
					CutoffTies.Sort(
						[&PressureDestinations](const int32 A, const int32 B)
						{
							const FPressureDestination& First =
								PressureDestinations[A];
							const FPressureDestination& Second =
								PressureDestinations[B];
							if (First.StableOrder != Second.StableOrder)
							{
								return First.StableOrder < Second.StableOrder;
							}
							return First.WorldCell.Y != Second.WorldCell.Y
								? First.WorldCell.Y < Second.WorldCell.Y
								: First.WorldCell.X < Second.WorldCell.X;
						});
					int64 Residual = PressureTransfer - PlacementsBelowCutoff;
					for (const int32 DestinationIndex : CutoffTies)
					{
						if (Residual <= 0)
						{
							break;
						}
						++PressureDestinations[DestinationIndex].TransferAmount;
						--Residual;
					}
					check(Residual == 0);
					RemainingToMove -= PressureTransfer;
				}

				// Only isolated liquid with no surrounding same-material capacity
				// reaches this path. Fill nearby empty columns compactly rather than
				// scattering visible one-particle shards across the whole radius.
				if (RemainingToMove > 0)
				{
					EmptyDestinations.Sort(
						[](const FEmptyDestination& A,
							const FEmptyDestination& B)
						{
							if (A.DistanceToFootprintBounds
								!= B.DistanceToFootprintBounds)
							{
								return A.DistanceToFootprintBounds
									< B.DistanceToFootprintBounds;
							}
							if (A.StableOrder != B.StableOrder)
							{
								return A.StableOrder < B.StableOrder;
							}
							return A.WorldCell.Y != B.WorldCell.Y
								? A.WorldCell.Y < B.WorldCell.Y
								: A.WorldCell.X < B.WorldCell.X;
						});
					for (FEmptyDestination& Destination : EmptyDestinations)
					{
						if (RemainingToMove <= 0)
						{
							break;
						}
						Destination.TransferAmount = static_cast<int32>(
							FMath::Min<int64>(RemainingToMove, FullCellAmount));
						RemainingToMove -= Destination.TransferAmount;
					}
				}

				const int64 TransferredAmount =
					InitialAmountToMove - RemainingToMove;
				if (TransferredAmount <= 0)
				{
					continue;
				}
				for (const FPressureDestination& Destination
					: PressureDestinations)
				{
					if (Destination.TransferAmount <= 0)
					{
						continue;
					}
					FImpl::FCell& DestinationCell =
						Impl->FindOrAddCell(Destination.WorldCell);
					const bool bDestinationWasEmpty =
						DestinationCell.MaterialIndex == 0;
					DestinationCell.MaterialIndex = MaterialIndex;
					if (TransferLifetime != 0)
					{
						DestinationCell.RemainingLifetime = bDestinationWasEmpty
							|| DestinationCell.RemainingLifetime == 0
							? TransferLifetime
							: FMath::Min(
								DestinationCell.RemainingLifetime,
								TransferLifetime);
					}
					DestinationCell.Amount = static_cast<uint8>(
						static_cast<int32>(DestinationCell.Amount)
							+ Destination.TransferAmount);
					DestinationCell.LastUpdatedTick = Impl->Tick;
					++Impl->LastLiquidDisplacementStats.DestinationCellsChanged;
					Impl->LastLiquidDisplacementStats.TransferredAmount +=
						Destination.TransferAmount;
					TouchedChunks.Add(
						Impl->ToChunkCoordinate(Destination.WorldCell));
					Impl->MarkDirtyNeighborhood(Destination.WorldCell);
				}
				for (const FEmptyDestination& Destination : EmptyDestinations)
				{
					if (Destination.TransferAmount <= 0)
					{
						continue;
					}
					FImpl::FCell& DestinationCell =
						Impl->FindOrAddCell(Destination.WorldCell);
					DestinationCell.MaterialIndex = MaterialIndex;
					DestinationCell.RemainingLifetime = TransferLifetime;
					DestinationCell.Amount =
						static_cast<uint8>(Destination.TransferAmount);
					DestinationCell.LastUpdatedTick = Impl->Tick;
					++Impl->LastLiquidDisplacementStats.DestinationCellsChanged;
					Impl->LastLiquidDisplacementStats.TransferredAmount +=
						Destination.TransferAmount;
					TouchedChunks.Add(
						Impl->ToChunkCoordinate(Destination.WorldCell));
					Impl->MarkDirtyNeighborhood(Destination.WorldCell);
				}

				int64 SourceAmountToConsume = TransferredAmount;
				for (const FSourceState& Source : Sources)
				{
					if (SourceAmountToConsume <= 0)
					{
						break;
					}
					FImpl::FCell* SourceCell = Impl->FindCell(Source.WorldCell);
					if (!SourceCell
						|| SourceCell->MaterialIndex != MaterialIndex
						|| SourceCell->Amount == 0)
					{
						continue;
					}
					const int32 ActualTransfer = static_cast<int32>(
						FMath::Min3<int64>(
							SourceAmountToConsume,
							Source.AmountToMove,
							SourceCell->Amount));
					if (ActualTransfer <= 0)
					{
						continue;
					}
					SourceCell->Amount = static_cast<uint8>(
						static_cast<int32>(SourceCell->Amount) - ActualTransfer);
					if (SourceCell->Amount == 0)
					{
						SourceCell->MaterialIndex = 0;
						SourceCell->RemainingLifetime = 0;
					}
					SourceCell->LastUpdatedTick = Impl->Tick;
					SourceCell->BodyDisplacedTick = Impl->Tick;
					SourceCell->BodyDisplacedMaterialIndex = MaterialIndex;
					SourceCell->BodyDisplacedReferenceAmount =
						Source.BodyDisplacedReferenceAmount;
					SourceCell->bHasBodyDisplacementVacancy = true;
					Impl->BodyDisplacementVacancies.Add(Source.WorldCell);
					TouchedChunks.Add(
						Impl->ToChunkCoordinate(Source.WorldCell));
					Impl->MarkDirtyNeighborhood(Source.WorldCell);
					SourceAmountToConsume -= ActualTransfer;
					++MovedCells;
				}
				check(SourceAmountToConsume == 0);
			}
		}

		for (const FIntPoint ChunkCoordinate : TouchedChunks)
		{
			if (!Impl->IsChunkActive(ChunkCoordinate))
			{
				Impl->ArchiveChunk(ChunkCoordinate);
			}
		}
		return MovedCells;
	}

	const FLiquidDisplacementStats&
	FChunkedMaterialWorld::GetLastLiquidDisplacementStats() const
	{
		return Impl->LastLiquidDisplacementStats;
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

	bool FChunkedMaterialWorld::TryGetCellSnapshot(
		const FIntPoint& WorldCell,
		FCellSnapshot& OutSnapshot) const
	{
		OutSnapshot = {};
		if (!Impl->bInitialized)
		{
			return false;
		}

		const FImpl::FCell* Cell = Impl->FindCell(WorldCell);
		const uint16 MaterialIndex = Cell
			? Cell->MaterialIndex
			: Impl->FindArchivedMaterialIndex(WorldCell);
		if (MaterialIndex == 0 || MaterialIndex >= Impl->Materials.Num())
		{
			return false;
		}

		int16 SupportHeight = 0;
		if (!Impl->FindSupportHeight(WorldCell, SupportHeight))
		{
			return false;
		}

		OutSnapshot.WorldCell = WorldCell;
		OutSnapshot.MaterialId = Impl->Materials[MaterialIndex].Id;
		OutSnapshot.SupportHeight = SupportHeight;
		OutSnapshot.Amount = Cell
			? Cell->Amount
			: Impl->FindArchivedAmount(WorldCell);
		OutSnapshot.RemainingLifetime = Cell
			? Cell->RemainingLifetime
			: 0;
		return true;
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

	int64 FChunkedMaterialWorld::SumMaterialAmount(
		const FName MaterialId) const
	{
		const uint16* MaterialIndex =
			Impl->MaterialIndices.Find(MaterialId);
		if (!MaterialIndex)
		{
			return 0;
		}

		int64 Total = 0;
		for (const TPair<FIntPoint, TUniquePtr<FImpl::FChunk>>& Pair
			: Impl->Chunks)
		{
			for (const FImpl::FCell& Cell : Pair.Value->Cells)
			{
				if (Cell.MaterialIndex == *MaterialIndex)
				{
					Total += Cell.Amount;
				}
			}
		}
		for (const TPair<FIntPoint, FImpl::FArchivedChunk>& Pair
			: Impl->ArchivedChunks)
		{
			for (const FImpl::FArchivedRun& Run : Pair.Value.Runs)
			{
				if (Run.MaterialIndex == *MaterialIndex)
				{
					Total += static_cast<int64>(Run.Amount) * Run.Length;
				}
			}
		}
		return Total;
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
				Snapshot.Amount = Pair.Value->Cells[CellIndex].Amount;
				Snapshot.RemainingLifetime =
					Pair.Value->Cells[CellIndex].RemainingLifetime;
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

	void FChunkedMaterialWorld::GetAllCells(
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
			for (int32 CellIndex = 0;
				CellIndex < Pair.Value->Cells.Num();
				++CellIndex)
			{
				const FImpl::FCell& Cell = Pair.Value->Cells[CellIndex];
				if (Cell.MaterialIndex == 0
					|| Cell.MaterialIndex >= Impl->Materials.Num())
				{
					continue;
				}

				FCellSnapshot& Snapshot = OutCells.AddDefaulted_GetRef();
				Snapshot.WorldCell = FIntPoint(
					Pair.Key.X * Impl->Settings.ChunkSize
						+ CellIndex % Impl->Settings.ChunkSize,
					Pair.Key.Y * Impl->Settings.ChunkSize
						+ CellIndex / Impl->Settings.ChunkSize);
				Snapshot.MaterialId = Impl->Materials[Cell.MaterialIndex].Id;
				Snapshot.SupportHeight = Cell.SupportHeight;
				Snapshot.Amount = Cell.Amount;
				Snapshot.RemainingLifetime = Cell.RemainingLifetime;
			}
		}

		for (const TPair<FIntPoint, FImpl::FArchivedChunk>& Pair
			: Impl->ArchivedChunks)
		{
			int32 CellIndex = 0;
			for (const FImpl::FArchivedRun& Run : Pair.Value.Runs)
			{
				for (int32 RunIndex = 0; RunIndex < Run.Length;
					++RunIndex, ++CellIndex)
				{
					if (Run.MaterialIndex == 0
						|| Run.MaterialIndex >= Impl->Materials.Num())
					{
						continue;
					}

					FCellSnapshot& Snapshot = OutCells.AddDefaulted_GetRef();
					Snapshot.WorldCell = FIntPoint(
						Pair.Key.X * Impl->Settings.ChunkSize
							+ CellIndex % Impl->Settings.ChunkSize,
						Pair.Key.Y * Impl->Settings.ChunkSize
							+ CellIndex / Impl->Settings.ChunkSize);
					Snapshot.MaterialId = Impl->Materials[Run.MaterialIndex].Id;
					Snapshot.SupportHeight = Run.SupportHeight;
					Snapshot.Amount = Run.Amount;
					Snapshot.RemainingLifetime = 0;
				}
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

	void FChunkedMaterialWorld::ConsumeProjectionDirtyChunks(
		TArray<FIntPoint>& OutChunks)
	{
		OutChunks.Reset();
		if (!Impl->bInitialized)
		{
			return;
		}
		OutChunks = Impl->ProjectionDirtyChunks.Array();
		Impl->ProjectionDirtyChunks.Reset();
		OutChunks.Sort([](const FIntPoint A, const FIntPoint B)
		{
			return A.Y != B.Y ? A.Y < B.Y : A.X < B.X;
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
			int16 BaselineSupportHeight = 0;
			uint16 Amount = FullCellAmount;
			uint8 RemainingLifetime = 0;
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
				const FIntPoint WorldCell(
					Pair.Key.X * Impl->Settings.ChunkSize
						+ CellIndex % Impl->Settings.ChunkSize,
					Pair.Key.Y * Impl->Settings.ChunkSize
						+ CellIndex / Impl->Settings.ChunkSize);
				uint16 BaselineMaterialIndex = 0;
				int16 BaselineSupportHeight = 0;
				uint16 BaselineAmount = 0;
				Impl->FindBaselineCell(
					WorldCell,
					BaselineMaterialIndex,
					BaselineSupportHeight,
					BaselineAmount);
				const uint16 CurrentAmount = Cell.MaterialIndex == 0
					? 0
					: Cell.Amount;
				BaselineAmount = BaselineMaterialIndex == 0
					? 0
					: BaselineAmount;
				if (Cell.MaterialIndex == BaselineMaterialIndex
					&& Cell.SupportHeight == BaselineSupportHeight
					&& CurrentAmount == BaselineAmount
					&& Cell.RemainingLifetime == 0)
				{
					continue;
				}
				Chunk.Cells.Add({
					static_cast<uint32>(CellIndex),
					Cell.MaterialIndex,
					Cell.SupportHeight,
					BaselineSupportHeight,
					CurrentAmount,
					Cell.RemainingLifetime });
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
			AppendVarUint32(OutState, ZigZagEncodeInt32(Focus.X));
			AppendVarUint32(OutState, ZigZagEncodeInt32(Focus.Y));
		}
		AppendVarUint32(
			OutState,
			static_cast<uint32>(ActiveChunks.Num()));
		for (const FActiveChunk& Chunk : ActiveChunks)
		{
			AppendVarUint32(
				OutState,
				ZigZagEncodeInt32(Chunk.Coordinate.X));
			AppendVarUint32(
				OutState,
				ZigZagEncodeInt32(Chunk.Coordinate.Y));
			struct FCellRun
			{
				uint32 Start = 0;
				uint32 Length = 0;
			};
			TArray<FCellRun> Runs;
			int32 DirectIndexBytes = 0;
			uint32 PreviousCellIndex = 0;
			bool bFirstCell = true;
			for (const FOccupiedCell& Cell : Chunk.Cells)
			{
				const uint32 Delta = bFirstCell
					? Cell.CellIndex
					: Cell.CellIndex - PreviousCellIndex;
				DirectIndexBytes += VarUint32ByteCount(Delta);
				if (Runs.IsEmpty()
					|| Cell.CellIndex
						!= Runs.Last().Start + Runs.Last().Length)
				{
					Runs.Add({ Cell.CellIndex, 1 });
				}
				else
				{
					++Runs.Last().Length;
				}
				PreviousCellIndex = Cell.CellIndex;
				bFirstCell = false;
			}
			int32 RunIndexBytes = VarUint32ByteCount(Runs.Num());
			uint32 PreviousRunEnd = 0;
			for (int32 RunIndex = 0; RunIndex < Runs.Num(); ++RunIndex)
			{
				const FCellRun& Run = Runs[RunIndex];
				const uint32 StartDelta = RunIndex == 0
					? Run.Start
					: Run.Start - PreviousRunEnd;
				RunIndexBytes += VarUint32ByteCount(StartDelta)
					+ VarUint32ByteCount(Run.Length);
				PreviousRunEnd = Run.Start + Run.Length;
			}
			const bool bUseRuns = RunIndexBytes < DirectIndexBytes;
			AppendVarUint32(
				OutState,
				(static_cast<uint32>(Chunk.Cells.Num()) << 1u)
					| (bUseRuns ? 1u : 0u));
			if (bUseRuns)
			{
				AppendVarUint32(OutState, Runs.Num());
				PreviousRunEnd = 0;
				for (int32 RunIndex = 0; RunIndex < Runs.Num(); ++RunIndex)
				{
					const FCellRun& Run = Runs[RunIndex];
					AppendVarUint32(
						OutState,
						RunIndex == 0
							? Run.Start
							: Run.Start - PreviousRunEnd);
					AppendVarUint32(OutState, Run.Length);
					PreviousRunEnd = Run.Start + Run.Length;
				}
			}
			else
			{
				PreviousCellIndex = 0;
				bFirstCell = true;
				for (const FOccupiedCell& Cell : Chunk.Cells)
				{
					AppendVarUint32(
						OutState,
						bFirstCell
							? Cell.CellIndex
							: Cell.CellIndex - PreviousCellIndex);
					PreviousCellIndex = Cell.CellIndex;
					bFirstCell = false;
				}
			}
			for (const FOccupiedCell& Cell : Chunk.Cells)
			{
				// Empty material unambiguously means zero amount. Omitting
				// that redundant byte matters for cuts and moving/reactive materials,
				// which can clear hundreds of baseline cells in one snapshot.
				const bool bHasPartialAmount =
					Cell.MaterialIndex != 0
					&& Cell.Amount != FullCellAmount;
				const bool bHasSupportDelta =
					Cell.SupportHeight != Cell.BaselineSupportHeight;
				const bool bHasLifetime = Cell.RemainingLifetime != 0;
				AppendVarUint32(
					OutState,
					(static_cast<uint32>(Cell.MaterialIndex) << 3u)
						| (bHasLifetime ? 4u : 0u)
						| (bHasSupportDelta ? 2u : 0u)
						| (bHasPartialAmount ? 1u : 0u));
				if (bHasSupportDelta)
				{
					AppendVarUint32(
						OutState,
						ZigZagEncodeInt32(
							static_cast<int32>(Cell.SupportHeight)
								- Cell.BaselineSupportHeight));
				}
				if (bHasPartialAmount)
				{
					AppendVarUint32(OutState, Cell.Amount);
				}
				if (bHasLifetime)
				{
					AppendUint8(OutState, Cell.RemainingLifetime);
				}
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
			uint16 Amount = FullCellAmount;
			uint8 RemainingLifetime = 0;
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
			|| Version != ActiveStateVersion
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
			bool bFocusRead = false;
			if (Version == ActiveStateVersion)
			{
				uint32 EncodedX = 0;
				uint32 EncodedY = 0;
				bFocusRead = ReadVarUint32(State, Offset, EncodedX)
					&& ReadVarUint32(State, Offset, EncodedY);
				if (bFocusRead)
				{
					Focus.X = ZigZagDecodeInt32(EncodedX);
					Focus.Y = ZigZagDecodeInt32(EncodedY);
				}
			}
			else
			{
				bFocusRead = ReadInt32(State, Offset, Focus.X)
					&& ReadInt32(State, Offset, Focus.Y);
			}
			if (!bFocusRead)
			{
				OutError = TEXT("Active material state focus list is truncated");
				return false;
			}
		}
		const bool bChunkCountRead = Version == ActiveStateVersion
			? ReadVarUint32(State, Offset, ChunkCount)
			: ReadUint32(State, Offset, ChunkCount);
		if (!bChunkCountRead
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
			bool bUsesCellRuns = false;
			bool bChunkHeaderRead = false;
			if (Version == ActiveStateVersion)
			{
				uint32 EncodedX = 0;
				uint32 EncodedY = 0;
				uint32 PackedCellHeader = 0;
				bChunkHeaderRead =
					ReadVarUint32(State, Offset, EncodedX)
					&& ReadVarUint32(State, Offset, EncodedY)
					&& ReadVarUint32(
						State, Offset, PackedCellHeader);
				if (bChunkHeaderRead)
				{
					Chunk.Coordinate.X = ZigZagDecodeInt32(EncodedX);
					Chunk.Coordinate.Y = ZigZagDecodeInt32(EncodedY);
					bUsesCellRuns = (PackedCellHeader & 1u) != 0;
					OccupiedCellCount = PackedCellHeader >> 1u;
				}
			}
			else
			{
				bChunkHeaderRead = ReadInt32(
						State, Offset, Chunk.Coordinate.X)
					&& ReadInt32(
						State, Offset, Chunk.Coordinate.Y)
					&& ReadUint32(
						State, Offset, OccupiedCellCount);
			}
			if (!bChunkHeaderRead
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
			TArray<uint32> CurrentCellIndices;
			if (Version == ActiveStateVersion)
			{
				CurrentCellIndices.Reserve(OccupiedCellCount);
				if (bUsesCellRuns)
				{
					uint32 RunCount = 0;
					uint32 PreviousRunEnd = 0;
					if (!ReadVarUint32(State, Offset, RunCount)
						|| RunCount == 0
						|| RunCount > OccupiedCellCount)
					{
						OutError = TEXT("Active material state contains an invalid cell-run count");
						return false;
					}
					for (uint32 RunIndex = 0; RunIndex < RunCount; ++RunIndex)
					{
						uint32 StartDelta = 0;
						uint32 RunLength = 0;
						if (!ReadVarUint32(State, Offset, StartDelta)
							|| !ReadVarUint32(State, Offset, RunLength)
							|| RunLength == 0
							|| (RunIndex > 0 && StartDelta == 0))
						{
							OutError = TEXT("Active material state contains an invalid cell run");
							return false;
						}
						const uint64 RunStart = RunIndex == 0
							? StartDelta
							: static_cast<uint64>(PreviousRunEnd) + StartDelta;
						const uint64 RunEnd = RunStart + RunLength;
						if (RunEnd > static_cast<uint64>(CellCount)
							|| CurrentCellIndices.Num() + RunLength
								> OccupiedCellCount)
						{
							OutError = TEXT("Active material state cell run exceeds its chunk");
							return false;
						}
						for (uint64 CellIndex = RunStart;
							CellIndex < RunEnd;
							++CellIndex)
						{
							CurrentCellIndices.Add(static_cast<uint32>(CellIndex));
						}
						PreviousRunEnd = static_cast<uint32>(RunEnd);
					}
				}
				else
				{
					uint32 PreviousIndex = 0;
					for (uint32 CellNumber = 0;
						CellNumber < OccupiedCellCount;
						++CellNumber)
					{
						uint32 Delta = 0;
						if (!ReadVarUint32(State, Offset, Delta)
							|| (CellNumber > 0 && Delta == 0)
							|| (CellNumber > 0
								&& PreviousIndex > MAX_uint32 - Delta))
						{
							OutError = TEXT("Active material state contains an invalid cell-index list");
							return false;
						}
						const uint32 CellIndex = CellNumber == 0
							? Delta
							: PreviousIndex + Delta;
						if (CellIndex >= static_cast<uint32>(CellCount))
						{
							OutError = TEXT("Active material state cell index exceeds its chunk");
							return false;
						}
						CurrentCellIndices.Add(CellIndex);
						PreviousIndex = CellIndex;
					}
				}
				if (CurrentCellIndices.Num()
					!= static_cast<int32>(OccupiedCellCount))
				{
					OutError = TEXT("Active material state cell runs do not match their declared count");
					return false;
				}
			}
			uint32 PreviousCellIndex = MAX_uint32;
			int32 PreviousSupportHeight = 0;
			for (uint32 CellNumber = 0;
				CellNumber < OccupiedCellCount;
				++CellNumber)
			{
				FImportedCell Cell;
				uint32 PackedMaterialCode = 0;
				bool bCellEncodingValid = true;
				if (Version == ActiveStateVersion)
				{
					Cell.CellIndex = CurrentCellIndices[CellNumber];
				}
				else if (Version >= CompactActiveStateVersion)
				{
					uint32 CellIndexDelta = 0;
					bCellEncodingValid = ReadVarUint32(
						State,
						Offset,
						CellIndexDelta);
					if (bCellEncodingValid)
					{
						if (CellNumber == 0)
						{
							Cell.CellIndex = CellIndexDelta;
						}
						else if (CellIndexDelta == 0
							|| PreviousCellIndex
								> MAX_uint32 - CellIndexDelta)
						{
							bCellEncodingValid = false;
						}
						else
						{
							Cell.CellIndex =
								PreviousCellIndex + CellIndexDelta;
						}
					}
				}
				else
				{
					bCellEncodingValid = ReadUint32(
						State,
						Offset,
						Cell.CellIndex);
				}
				if (bCellEncodingValid
					&& Version >= CompactActiveStateVersion)
				{
					bCellEncodingValid = ReadVarUint32(
						State,
						Offset,
						PackedMaterialCode);
				}
				else if (bCellEncodingValid)
				{
					uint16 LegacyMaterialIndex = 0;
					bCellEncodingValid = ReadUint16(
						State,
						Offset,
						LegacyMaterialIndex);
					PackedMaterialCode = LegacyMaterialIndex;
				}
				if (bCellEncodingValid
					&& Version >= BaselineDeltaActiveStateVersion)
				{
					bCellEncodingValid =
						Cell.CellIndex < static_cast<uint32>(CellCount);
					if (bCellEncodingValid)
					{
						const FIntPoint WorldCell(
							Chunk.Coordinate.X * Impl->Settings.ChunkSize
								+ static_cast<int32>(Cell.CellIndex)
									% Impl->Settings.ChunkSize,
							Chunk.Coordinate.Y * Impl->Settings.ChunkSize
								+ static_cast<int32>(Cell.CellIndex)
									/ Impl->Settings.ChunkSize);
						uint16 BaselineMaterial = 0;
						int16 BaselineSupport = 0;
						uint16 BaselineAmount = 0;
						Impl->FindBaselineCell(
							WorldCell,
							BaselineMaterial,
							BaselineSupport,
							BaselineAmount);
						Cell.SupportHeight = BaselineSupport;
						if ((PackedMaterialCode & 2u) != 0)
						{
							uint32 EncodedSupportDelta = 0;
							bCellEncodingValid = ReadVarUint32(
								State,
								Offset,
								EncodedSupportDelta);
							if (bCellEncodingValid)
							{
								const int64 DecodedSupport =
									static_cast<int64>(BaselineSupport)
									+ ZigZagDecodeInt32(
										EncodedSupportDelta);
								bCellEncodingValid =
									DecodedSupport >= MIN_int16
									&& DecodedSupport <= MAX_int16;
								if (bCellEncodingValid)
								{
									Cell.SupportHeight =
										static_cast<int16>(DecodedSupport);
								}
							}
						}
					}
				}
				else if (bCellEncodingValid
					&& Version == CompactActiveStateVersion)
				{
					uint32 EncodedSupportDelta = 0;
					bCellEncodingValid = ReadVarUint32(
						State,
						Offset,
						EncodedSupportDelta);
					if (bCellEncodingValid)
					{
						const int64 DecodedSupport =
							static_cast<int64>(PreviousSupportHeight)
							+ ZigZagDecodeInt32(EncodedSupportDelta);
						bCellEncodingValid =
							DecodedSupport >= MIN_int16
							&& DecodedSupport <= MAX_int16;
						if (bCellEncodingValid)
						{
							Cell.SupportHeight =
								static_cast<int16>(DecodedSupport);
						}
					}
				}
				else if (bCellEncodingValid)
				{
					bCellEncodingValid = ReadInt16(
						State,
						Offset,
						Cell.SupportHeight);
				}
				if (bCellEncodingValid
					&& Version >= CompactActiveStateVersion
					&& (PackedMaterialCode & 1u) != 0)
				{
					uint32 EncodedAmount = 0;
					bCellEncodingValid = ReadVarUint32(
						State,
						Offset,
						EncodedAmount)
						&& EncodedAmount <= MAX_uint16;
					if (bCellEncodingValid)
					{
						Cell.Amount = static_cast<uint16>(EncodedAmount);
					}
				}
				else if (bCellEncodingValid
					&& Version == ActiveStateVersion
					&& (PackedMaterialCode >> 3u) == 0)
				{
					Cell.Amount = 0;
				}
				if (bCellEncodingValid
					&& Version == ActiveStateVersion
					&& (PackedMaterialCode & 4u) != 0)
				{
					bCellEncodingValid = ReadUint8(
						State,
						Offset,
						Cell.RemainingLifetime)
						&& Cell.RemainingLifetime != 0;
				}
				if (!bCellEncodingValid
					|| Cell.CellIndex >= static_cast<uint32>(CellCount)
					|| (CellNumber > 0
						&& Cell.CellIndex <= PreviousCellIndex))
				{
					OutError =
						TEXT("Active material state contains an invalid occupied cell");
					return false;
				}
				const uint32 DecodedMaterialIndex =
					Version == ActiveStateVersion
						? PackedMaterialCode >> 3u
						: Version >= BaselineDeltaActiveStateVersion
						? PackedMaterialCode >> 2u
						: Version == CompactActiveStateVersion
						? PackedMaterialCode >> 1u
						: PackedMaterialCode;
				Cell.MaterialIndex = static_cast<uint16>(DecodedMaterialIndex);
				const bool bInvalidEmptyEncoding =
					Version < BaselineDeltaActiveStateVersion
						? Cell.MaterialIndex == 0 || Cell.Amount == 0
						: (Cell.MaterialIndex == 0) != (Cell.Amount == 0);
				if (DecodedMaterialIndex > MaterialIndexMask
					|| Cell.MaterialIndex >= Impl->Materials.Num()
					|| bInvalidEmptyEncoding)
				{
					OutError = FString::Printf(
						TEXT("Active material state contains invalid material code: version=%u chunk=(%d,%d) cellNumber=%u cellIndex=%u packed=%u decoded=%u amount=%u materialCount=%d"),
						Version,
						Chunk.Coordinate.X,
						Chunk.Coordinate.Y,
						CellNumber,
						Cell.CellIndex,
						PackedMaterialCode,
						DecodedMaterialIndex,
						Cell.Amount,
						Impl->Materials.Num());
					return false;
				}
				if (Cell.MaterialIndex != 0)
				{
					const uint8 DefinedLifetime =
						Impl->Materials[Cell.MaterialIndex].LifetimeSteps;
					if ((DefinedLifetime == 0) != (Cell.RemainingLifetime == 0)
						|| Cell.RemainingLifetime > DefinedLifetime)
					{
						OutError = TEXT("Active material state contains an invalid material lifetime");
						return false;
					}
				}
				PreviousCellIndex = Cell.CellIndex;
				PreviousSupportHeight = Cell.SupportHeight;
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

		Impl->BodyDisplacementVacancies.Reset();
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
				if (Version >= BaselineDeltaActiveStateVersion)
				{
					Cell.SupportHeight = 0;
				}
				Cell.Amount = 0;
				Cell.RemainingLifetime = 0;
				Cell.LastUpdatedTick = 0;
				Cell.BodyDisplacedTick = 0;
				Cell.BodyDisplacedMaterialIndex = 0;
				Cell.BodyDisplacedReferenceAmount = 0;
				Cell.bHasBodyDisplacementVacancy = false;
			}
			if (Version >= BaselineDeltaActiveStateVersion)
			{
				if (const FImpl::FArchivedChunk* Baseline =
					Impl->BaselineChunks.Find(Pair.Key))
				{
					Impl->DecodeChunk(*Baseline, *Pair.Value);
				}
			}
			Pair.Value->DirtyMin = FIntPoint(0, 0);
			Pair.Value->DirtyMax = FIntPoint(
				Impl->Settings.ChunkSize - 1,
				Impl->Settings.ChunkSize - 1);
			Pair.Value->bHasDirtyCells = true;
			Impl->ProjectionDirtyChunks.Add(Pair.Key);
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
				Cell.Amount = ImportedCell.Amount;
				Cell.RemainingLifetime = ImportedCell.RemainingLifetime;
				Cell.LastUpdatedTick = 0;
				Cell.BodyDisplacedTick = 0;
				Cell.BodyDisplacedMaterialIndex = 0;
				Cell.BodyDisplacedReferenceAmount = 0;
				Cell.bHasBodyDisplacementVacancy = false;
			}
			Chunk.DirtyMin = FIntPoint(0, 0);
			Chunk.DirtyMax = FIntPoint(
				Impl->Settings.ChunkSize - 1,
				Impl->Settings.ChunkSize - 1);
			Chunk.bHasDirtyCells = true;
			Impl->ProjectionDirtyChunks.Add(ImportedChunk.Coordinate);
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
		TRACE_CPUPROFILER_EVENT_SCOPE(MatterFlux_MaterialWorld_SetFocuses);
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

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(
				MatterFlux_MaterialWorld_ArchiveDepartedChunks);
			TArray<FIntPoint> ChunksToArchive;
			for (const TPair<FIntPoint, TUniquePtr<FImpl::FChunk>>& Pair
				: Impl->Chunks)
			{
				if (!Impl->IsChunkSimulated(Pair.Key))
				{
					ChunksToArchive.Add(Pair.Key);
				}
			}
			for (const FIntPoint& ChunkCoordinate : ChunksToArchive)
			{
				Impl->ArchiveChunk(ChunkCoordinate);
			}
		}

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(
				MatterFlux_MaterialWorld_RestoreArrivingChunks);
			TArray<FIntPoint> ChunksToRestore;
			for (const TPair<FIntPoint, FImpl::FArchivedChunk>& Pair
				: Impl->ArchivedChunks)
			{
				if (Impl->IsChunkSimulated(Pair.Key))
				{
					ChunksToRestore.Add(Pair.Key);
				}
			}
			for (const FIntPoint& ChunkCoordinate : ChunksToRestore)
			{
				Impl->FindOrAddChunk(ChunkCoordinate);
				Impl->MarkChunkForBaselineResume(ChunkCoordinate);
			}
		}

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(
				MatterFlux_MaterialWorld_WakeArrivingSeams);
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
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(
				MatterFlux_MaterialStep_GatherDirtyCells);
			for (const TPair<FIntPoint, TUniquePtr<FImpl::FChunk>>& Pair
				: Impl->Chunks)
			{
				FImpl::FChunk& Chunk = *Pair.Value;
				if (!Impl->IsChunkSimulated(Pair.Key) || !Chunk.bHasDirtyCells)
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
		}

		// Lifetime is a material property, independent of phase and reaction kind.
		// Age each cell once at the start of a fixed step so movement cannot extend
		// its lifetime by crossing a chunk or changing traversal order.
		for (const FIntPoint& WorldCell : CellsToVisit)
		{
			FImpl::FCell* Cell = Impl->FindCell(WorldCell);
			if (!Cell
				|| Cell->MaterialIndex == 0
				|| Cell->RemainingLifetime == 0)
			{
				continue;
			}
			--Cell->RemainingLifetime;
			if (Cell->RemainingLifetime == 0)
			{
				Cell->MaterialIndex = 0;
				Cell->Amount = 0;
				Impl->MarkDirtyNeighborhood(WorldCell);
				++Stats.CulledCells;
			}
			else
			{
				Impl->MarkDirty(WorldCell);
			}
		}

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

		// Body displacement is a short-lived volume constraint on a world
		// column. Relax those sparse vacancies explicitly before ordinary liquid
		// motion. This lets a wake refill across an uneven lake bed without
		// treating every adjacent river/lake support height as one global vessel.
		if (Impl->Settings.bUseSurfaceTopology
			&& !Impl->BodyDisplacementVacancies.IsEmpty())
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(
				MatterFlux_MaterialStep_RestoreBodyDisplacement);
			static const FIntPoint RefillDirections[] =
			{
				FIntPoint(1, 0), FIntPoint(0, 1),
				FIntPoint(-1, 0), FIntPoint(0, -1),
				FIntPoint(1, 1), FIntPoint(-1, 1),
				FIntPoint(-1, -1), FIntPoint(1, -1)
			};
			TArray<FIntPoint> Vacancies =
				Impl->BodyDisplacementVacancies.Array();
			Vacancies.Sort([](const FIntPoint A, const FIntPoint B)
			{
				return A.Y != B.Y ? A.Y < B.Y : A.X < B.X;
			});
			struct FRestitutionVacancy
			{
				FIntPoint WorldCell = FIntPoint::ZeroValue;
				uint16 MaterialIndex = 0;
				int32 RemainingTransfer = 0;
			};
			TArray<FRestitutionVacancy> RestitutionVacancies;
			RestitutionVacancies.Reserve(Vacancies.Num());
			for (const FIntPoint Vacancy : Vacancies)
			{
				FImpl::FCell* DestinationCell = Impl->FindCell(Vacancy);
				if (!DestinationCell
					|| !DestinationCell->bHasBodyDisplacementVacancy
					|| DestinationCell->BodyDisplacedTick > Impl->Tick
					|| Impl->Tick - DestinationCell->BodyDisplacedTick
						> MaximumBodyDisplacementRefillTicks)
				{
					if (DestinationCell)
					{
						DestinationCell->bHasBodyDisplacementVacancy = false;
						DestinationCell->BodyDisplacedMaterialIndex = 0;
						DestinationCell->BodyDisplacedReferenceAmount = 0;
					}
					Impl->BodyDisplacementVacancies.Remove(Vacancy);
					continue;
				}
				const uint16 DisplacedMaterialIndex =
					DestinationCell->MaterialIndex != 0
						? DestinationCell->MaterialIndex
						: DestinationCell->BodyDisplacedMaterialIndex;
				if (DestinationCell->BodyDisplacedReferenceAmount == 0)
				{
					if (DestinationCell->Amount >= FullCellAmount
						|| DisplacedMaterialIndex == 0
						|| DisplacedMaterialIndex >= Impl->Materials.Num()
						|| Impl->Materials[DisplacedMaterialIndex].Phase
							!= EMatterFluxMaterialPhase::Liquid)
					{
						continue;
					}
					int16 DestinationSupport = 0;
					if (!Impl->FindSupportHeight(Vacancy, DestinationSupport))
					{
						continue;
					}
					const int64 DestinationSurface255 =
						static_cast<int64>(DestinationSupport) * FullCellAmount
							+ static_cast<int64>(
								Impl->Settings.LiquidFullColumnHeight)
								* DestinationCell->Amount;
					FIntPoint BestLocalSource = Vacancy;
					int64 BestLocalSurface255 = DestinationSurface255;
					for (const FIntPoint Direction : RefillDirections)
					{
						FIntPoint Candidate;
						if (!TryOffsetCell(Vacancy, Direction, Candidate))
						{
							continue;
						}
						const FImpl::FCell* CandidateCell = Impl->FindCell(Candidate);
						if (!CandidateCell
							|| CandidateCell->MaterialIndex != DisplacedMaterialIndex
							|| CandidateCell->Amount == 0)
						{
							continue;
						}
						int16 CandidateSupport = 0;
						if (!Impl->FindSupportHeight(Candidate, CandidateSupport))
						{
							continue;
						}
						const int64 CandidateSurface255 =
							static_cast<int64>(CandidateSupport) * FullCellAmount
								+ static_cast<int64>(
									Impl->Settings.LiquidFullColumnHeight)
									* CandidateCell->Amount;
						if (CandidateSurface255 > BestLocalSurface255)
						{
							BestLocalSurface255 = CandidateSurface255;
							BestLocalSource = Candidate;
						}
					}
					if (BestLocalSource == Vacancy)
					{
						continue;
					}
					FImpl::FCell* SourceCell = Impl->FindCell(BestLocalSource);
					const int32 SurfaceDifferenceAmount = static_cast<int32>(
						(BestLocalSurface255 - DestinationSurface255)
							/ Impl->Settings.LiquidFullColumnHeight);
					const int32 TransferAmount = FMath::Min3(
						FMath::Max(SurfaceDifferenceAmount / 2, 1),
						MaximumLiquidTransferPerStep,
						FMath::Min(
							static_cast<int32>(SourceCell->Amount),
							FullCellAmount - static_cast<int32>(DestinationCell->Amount)));
					if (TransferAmount <= 0)
					{
						continue;
					}
					DestinationCell->MaterialIndex = DisplacedMaterialIndex;
					if (SourceCell->RemainingLifetime != 0)
					{
						DestinationCell->RemainingLifetime =
							DestinationCell->RemainingLifetime == 0
							? SourceCell->RemainingLifetime
							: FMath::Min(
								DestinationCell->RemainingLifetime,
								SourceCell->RemainingLifetime);
					}
					DestinationCell->Amount = static_cast<uint8>(
						static_cast<int32>(DestinationCell->Amount) + TransferAmount);
					SourceCell->Amount = static_cast<uint8>(
						static_cast<int32>(SourceCell->Amount) - TransferAmount);
					if (SourceCell->Amount == 0)
					{
						SourceCell->MaterialIndex = 0;
						SourceCell->RemainingLifetime = 0;
					}
					DestinationCell->LastUpdatedTick = Impl->Tick;
					SourceCell->LastUpdatedTick = Impl->Tick;
					Impl->MarkDirtyNeighborhood(Vacancy);
					Impl->MarkDirtyNeighborhood(BestLocalSource);
					++Stats.MovedCells;
					continue;
				}
				if (DestinationCell->Amount
					>= DestinationCell->BodyDisplacedReferenceAmount)
				{
					continue;
				}
				if (DisplacedMaterialIndex == 0
					|| DisplacedMaterialIndex >= Impl->Materials.Num()
					|| Impl->Materials[DisplacedMaterialIndex].Phase
						!= EMatterFluxMaterialPhase::Liquid)
				{
					continue;
				}
				RestitutionVacancies.Add({
					Vacancy,
					DisplacedMaterialIndex,
					FMath::Min(
						static_cast<int32>(
							DestinationCell->BodyDisplacedReferenceAmount)
							- static_cast<int32>(DestinationCell->Amount),
						MaximumLiquidTransferPerStep) });
			}

			// Noita's falling-material update visits a dirty region once rather than
			// running one neighborhood search per pixel. Apply the same invariant to
			// body-wake restitution: discover each affected liquid component once,
			// then match its deficits and conserved surplus without another flood.
			TArray<uint16> RestitutionMaterials;
			for (const FRestitutionVacancy& Vacancy : RestitutionVacancies)
			{
				RestitutionMaterials.AddUnique(Vacancy.MaterialIndex);
			}
			RestitutionMaterials.Sort();
			for (const uint16 RestitutionMaterialIndex : RestitutionMaterials)
			{
				struct FRestitutionDonor
				{
					FIntPoint WorldCell = FIntPoint::ZeroValue;
					int32 AvailableAmount = 0;
					int64 SurfaceHeight255 = 0;
				};
				TMap<FIntPoint, int32> VacancyIndicesByCell;
				VacancyIndicesByCell.Reserve(RestitutionVacancies.Num());
				for (int32 VacancyIndex = 0;
					VacancyIndex < RestitutionVacancies.Num();
					++VacancyIndex)
				{
					if (RestitutionVacancies[VacancyIndex].MaterialIndex
						== RestitutionMaterialIndex)
					{
						VacancyIndicesByCell.Add(
							RestitutionVacancies[VacancyIndex].WorldCell,
							VacancyIndex);
					}
				}

				TSet<FIntPoint> SearchVisited;
				SearchVisited.Reserve(RestitutionVacancies.Num() * 8);
				constexpr int32 MaximumBatchedRefillSearchCells = 131072;
				for (int32 SeedVacancyIndex = 0;
					SeedVacancyIndex < RestitutionVacancies.Num();
					++SeedVacancyIndex)
				{
					const FRestitutionVacancy& SeedVacancy =
						RestitutionVacancies[SeedVacancyIndex];
					if (SeedVacancy.MaterialIndex != RestitutionMaterialIndex
						|| SearchVisited.Contains(SeedVacancy.WorldCell))
					{
						continue;
					}

					TArray<FIntPoint> SearchQueue;
					TArray<int32> ComponentVacancyIndices;
					TArray<FRestitutionDonor> ComponentDonors;
					SearchQueue.Reserve(RestitutionVacancies.Num() * 4);
					SearchQueue.Add(SeedVacancy.WorldCell);
					SearchVisited.Add(SeedVacancy.WorldCell);
					for (int32 SearchIndex = 0;
						SearchIndex < SearchQueue.Num()
							&& SearchQueue.Num()
								<= MaximumBatchedRefillSearchCells;
						++SearchIndex)
					{
						const FIntPoint Current = SearchQueue[SearchIndex];
						if (const int32* VacancyIndex =
							VacancyIndicesByCell.Find(Current))
						{
							ComponentVacancyIndices.Add(*VacancyIndex);
						}
						for (int32 DirectionIndex = 0; DirectionIndex < 4;
							++DirectionIndex)
						{
							FIntPoint Candidate;
							if (!TryOffsetCell(
									Current,
									RefillDirections[DirectionIndex],
									Candidate)
								|| SearchVisited.Contains(Candidate))
							{
								continue;
							}
							FImpl::FCell* CandidateCell = Impl->FindCell(Candidate);
							const bool bSameLiquid = CandidateCell
								&& CandidateCell->MaterialIndex
									== RestitutionMaterialIndex
								&& CandidateCell->Amount > 0;
							const bool bSameMaterialVacancy = CandidateCell
								&& CandidateCell->bHasBodyDisplacementVacancy
								&& CandidateCell->BodyDisplacedMaterialIndex
									== RestitutionMaterialIndex;
							if (!bSameLiquid && !bSameMaterialVacancy)
							{
								continue;
							}
							SearchVisited.Add(Candidate);
							SearchQueue.Add(Candidate);
						}
					}

					for (const FIntPoint Candidate : SearchQueue)
					{
						FImpl::FCell* SourceCell = Impl->FindCell(Candidate);
						if (!SourceCell
							|| SourceCell->MaterialIndex
								!= RestitutionMaterialIndex
							|| SourceCell->Amount == 0)
						{
							continue;
						}
						int32 RetainedSourceAmount = 0;
						if (SourceCell->bHasBodyDisplacementVacancy
							&& SourceCell->BodyDisplacedMaterialIndex
								== RestitutionMaterialIndex)
						{
							RetainedSourceAmount =
								SourceCell->BodyDisplacedReferenceAmount;
						}
						else
						{
							uint16 BaselineMaterialIndex = 0;
							int16 BaselineSupportHeight = 0;
							uint16 BaselineAmount = 0;
							if (Impl->FindBaselineCell(
									Candidate,
									BaselineMaterialIndex,
									BaselineSupportHeight,
									BaselineAmount)
								&& BaselineMaterialIndex == RestitutionMaterialIndex)
							{
								RetainedSourceAmount = BaselineAmount;
							}
						}
						const int32 AvailableSourceAmount =
							static_cast<int32>(SourceCell->Amount)
								- RetainedSourceAmount;
						if (AvailableSourceAmount > 0)
						{
							int16 SourceSupport = 0;
							Impl->FindSupportHeight(Candidate, SourceSupport);
							ComponentDonors.Add({
								Candidate,
								AvailableSourceAmount,
								static_cast<int64>(SourceSupport) * FullCellAmount
									+ static_cast<int64>(
										Impl->Settings.LiquidFullColumnHeight)
										* SourceCell->Amount });
						}
					}

					for (const int32 VacancyIndex : ComponentVacancyIndices)
					{
						FRestitutionVacancy& Destination =
							RestitutionVacancies[VacancyIndex];
						int32 BestDonorIndex = INDEX_NONE;
						int64 BestDistance = MAX_int64;
						int64 BestSurfaceHeight255 = MIN_int64;
						for (int32 DonorIndex = 0;
							DonorIndex < ComponentDonors.Num();
							++DonorIndex)
						{
							const FRestitutionDonor& Donor =
								ComponentDonors[DonorIndex];
							if (Donor.AvailableAmount <= 0)
							{
								continue;
							}
							const int64 Distance =
								FMath::Abs(
									static_cast<int64>(Donor.WorldCell.X)
										- Destination.WorldCell.X)
								+ FMath::Abs(
									static_cast<int64>(Donor.WorldCell.Y)
										- Destination.WorldCell.Y);
							if (Distance < BestDistance
								|| (Distance == BestDistance
									&& Donor.SurfaceHeight255
										> BestSurfaceHeight255))
							{
								BestDonorIndex = DonorIndex;
								BestDistance = Distance;
								BestSurfaceHeight255 = Donor.SurfaceHeight255;
							}
						}
						if (BestDonorIndex == INDEX_NONE)
						{
							continue;
						}

						FRestitutionDonor& Donor =
							ComponentDonors[BestDonorIndex];
						FImpl::FCell* DestinationCell =
							Impl->FindCell(Destination.WorldCell);
						FImpl::FCell* SourceCell =
							Impl->FindCell(Donor.WorldCell);
						if (!DestinationCell || !SourceCell)
						{
							continue;
						}
						const int32 TransferAmount = FMath::Min(
							Destination.RemainingTransfer,
							Donor.AvailableAmount);
						if (TransferAmount <= 0)
						{
							continue;
						}
						DestinationCell->MaterialIndex = RestitutionMaterialIndex;
						if (SourceCell->RemainingLifetime != 0)
						{
							DestinationCell->RemainingLifetime =
								DestinationCell->RemainingLifetime == 0
								? SourceCell->RemainingLifetime
								: FMath::Min(
									DestinationCell->RemainingLifetime,
									SourceCell->RemainingLifetime);
						}
						DestinationCell->Amount = static_cast<uint8>(
							static_cast<int32>(DestinationCell->Amount)
								+ TransferAmount);
						SourceCell->Amount = static_cast<uint8>(
							static_cast<int32>(SourceCell->Amount)
								- TransferAmount);
						if (SourceCell->Amount == 0)
						{
							SourceCell->MaterialIndex = 0;
							SourceCell->RemainingLifetime = 0;
						}
						Destination.RemainingTransfer -= TransferAmount;
						Donor.AvailableAmount -= TransferAmount;
						DestinationCell->LastUpdatedTick = Impl->Tick;
						SourceCell->LastUpdatedTick = Impl->Tick;
						Impl->MarkDirtyNeighborhood(Destination.WorldCell);
						Impl->MarkDirtyNeighborhood(Donor.WorldCell);
						++Stats.MovedCells;
					}
				}
				Stats.RestitutionVisitedCells += SearchVisited.Num();
			}
		}

		TMap<FIntPoint, FImpl::FSurfacePuddleShape> SurfaceLiquidShapes;
		if (Impl->Settings.bUseSurfaceTopology)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(
				MatterFlux_MaterialStep_ClassifySurfaceLiquids);
			static const FIntPoint NeighborDirections[] =
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
			TSet<FIntPoint> ClassifiedCells;
			for (const FIntPoint Root : CellsToVisit)
			{
				const FImpl::FCell* RootCell = Impl->FindCell(Root);
				if (!RootCell
					|| RootCell->MaterialIndex == 0
					|| ClassifiedCells.Contains(Root)
					|| Impl->Materials[RootCell->MaterialIndex].Phase
						!= EMatterFluxMaterialPhase::Liquid)
				{
					continue;
				}

				TArray<FIntPoint> Component;
				Component.Reserve(32);
				Component.Add(Root);
				ClassifiedCells.Add(Root);
				FIntPoint Minimum = Root;
				FIntPoint Maximum = Root;
				int32 TotalAmount = RootCell->Amount;
				double WeightedX = static_cast<double>(Root.X)
					* RootCell->Amount;
				double WeightedY = static_cast<double>(Root.Y)
					* RootCell->Amount;
				for (int32 QueueIndex = 0;
					QueueIndex < Component.Num();
					++QueueIndex)
				{
					const FIntPoint Current = Component[QueueIndex];
					for (const FIntPoint Direction : NeighborDirections)
					{
						FIntPoint Neighbor;
						if (!TryOffsetCell(Current, Direction, Neighbor)
							|| ClassifiedCells.Contains(Neighbor))
						{
							continue;
						}
						const FImpl::FCell* NeighborCell =
							Impl->FindCell(Neighbor);
						if (!NeighborCell
							|| NeighborCell->MaterialIndex
								!= RootCell->MaterialIndex)
						{
							continue;
						}
						ClassifiedCells.Add(Neighbor);
						Component.Add(Neighbor);
						TotalAmount += NeighborCell->Amount;
						WeightedX += static_cast<double>(Neighbor.X)
							* NeighborCell->Amount;
						WeightedY += static_cast<double>(Neighbor.Y)
							* NeighborCell->Amount;
						Minimum.X = FMath::Min(Minimum.X, Neighbor.X);
						Minimum.Y = FMath::Min(Minimum.Y, Neighbor.Y);
						Maximum.X = FMath::Max(Maximum.X, Neighbor.X);
						Maximum.Y = FMath::Max(Maximum.Y, Neighbor.Y);
					}
				}
				const int32 Width = Maximum.X - Minimum.X + 1;
				const int32 Height = Maximum.Y - Minimum.Y + 1;
				const int8 AxisBias = Height >= Width + 2
					? -1
					: (Width >= Height + 2 ? 1 : 0);
				constexpr int32 MaximumRoundedPuddleCells = 256;
				if (Component.Num() <= MaximumRoundedPuddleCells
					&& TotalAmount > 0)
				{
					FImpl::FSurfacePuddleShape Shape;
					Shape.Center = FVector2D(
						WeightedX / static_cast<double>(TotalAmount),
						WeightedY / static_cast<double>(TotalAmount));
					// 约半格液量形成一个可见表面格；由总液量推导圆盘面积，
					// 让形状收敛与初始矩形朝向无关。
					constexpr float TargetAverageAmount = 128.0f;
					const float TargetArea = static_cast<float>(TotalAmount)
						/ TargetAverageAmount;
					Shape.Radius = FMath::Sqrt(
						FMath::Max(TargetArea / UE_PI, 1.0f));
					Shape.AxisBias = AxisBias;
					for (const FIntPoint Cell : Component)
					{
						SurfaceLiquidShapes.Add(Cell, Shape);
					}
				}
			}
		}

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(MatterFlux_MaterialStep_MoveCells);
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
						Stats,
						SurfaceLiquidShapes.Find(WorldCell));
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
		}
		if (Impl->Settings.bUseSurfaceTopology
			&& !Impl->SurfaceFlowChunks.IsEmpty())
		{
			TArray<FIntPoint> SettledFlowChunks;
			for (const FIntPoint ChunkCoordinate : Impl->SurfaceFlowChunks)
			{
				if (Impl->IsChunkActive(ChunkCoordinate))
				{
					continue;
				}
				const TUniquePtr<FImpl::FChunk>* Chunk =
					Impl->Chunks.Find(ChunkCoordinate);
				if (!Chunk || !Chunk->IsValid()
					|| !(**Chunk).bHasDirtyCells)
				{
					SettledFlowChunks.Add(ChunkCoordinate);
				}
			}
			for (const FIntPoint ChunkCoordinate : SettledFlowChunks)
			{
				Impl->SurfaceFlowChunks.Remove(ChunkCoordinate);
				Impl->ArchiveChunk(ChunkCoordinate);
			}
		}
		return Stats;
	}
}
