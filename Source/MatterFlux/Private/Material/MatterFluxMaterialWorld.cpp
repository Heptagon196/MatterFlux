#include "Material/MatterFluxMaterialWorld.h"
#include "Material/MatterFluxLocalMaterialReaction.h"
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
		constexpr uint16 ActiveStateVersion = 9;
		constexpr int32 MaximumActiveStateBytes = 1024 * 1024;
		constexpr uint16 FullCellAmount = 255;
		/** World-space pile height represented by one full powder unit. */
		constexpr int32 PowderFullColumnHeight = SurfacePowderFullColumnHeight;
		constexpr uint8 MinimumLiquidEdgeAmount = 24;
		constexpr int32 MaximumLiquidTransferPerStep = 72;
		constexpr uint8 FreeFlowTransitionStart = 210;
		constexpr uint8 FreeFlowTransitionEnd = 220;

		float GetLiquidFreeFlowBlend(const uint8 Dispersion)
		{
			return FMath::Clamp(
				(static_cast<float>(Dispersion) - FreeFlowTransitionStart)
					/ static_cast<float>(
						FreeFlowTransitionEnd - FreeFlowTransitionStart),
				0.0f,
				1.0f);
		}

		int32 GetLiquidTargetAverageAmount(const uint8 Dispersion)
		{
			// Water should settle as a shallow sheet while viscous liquids retain
			// a deeper mound. Previously every liquid targeted a half-full 64 cm
			// column, which made a small spell payload stand upright on flat ground.
			constexpr int32 ShallowTargetAmount = 8;
			constexpr int32 ViscousTargetAmount = 128;
			// Only freely flowing liquids should target a shallow sheet. Acid and
			// other cohesive liquids retain the original compact-puddle depth; the
			// 210..220 transition lets content authors opt into water-like flow
			// without a discontinuity. Once the freely flowing threshold is reached,
			// use the authored shallow target directly instead of retaining a residual
			// fraction of the viscous depth.
			return FMath::Clamp(
				FMath::RoundToInt(FMath::Lerp(
					static_cast<float>(ViscousTargetAmount),
					static_cast<float>(ShallowTargetAmount),
					GetLiquidFreeFlowBlend(Dispersion))),
				ShallowTargetAmount,
				ViscousTargetAmount);
		}

		int32 GetLiquidEdgeTransferAmount(const uint8 Dispersion)
		{
			return FMath::Clamp(
				FMath::RoundToInt(
					static_cast<float>(MinimumLiquidEdgeAmount)
						* (255.0f - static_cast<float>(Dispersion)) / 255.0f),
				1,
				MinimumLiquidEdgeAmount);
		}
		// A vacancy is a canonical, short-lived body/material interaction fact.
		// Runtime creatures can traverse a large basin for hundreds of simulation
		// steps before their earliest wake is free to settle. Expiring after 64
		// steps discarded that fact mid-traversal and left permanent empty/low
		// columns on uneven terrain. This remains bounded, but spans a complete
		// long traversal and its restitution window.
		constexpr uint32 MaximumBodyDisplacementRefillTicks = 512;

		int32 GetBodyWakeTransferBudget(
			const int32 Deficit,
			const int32 ReferenceAmount,
			const int32 RefillDurationSteps)
		{
			if (Deficit <= 0 || ReferenceAmount <= 0)
			{
				return 0;
			}
			// Delay and duration are separate facts. Once the hold interval ends,
			// divide the reference column across the authored duration rather than
			// front-loading most of the refill into the first pressure response.
			return FMath::Min(
				Deficit,
				FMath::Max(
					FMath::DivideAndRoundUp(
						ReferenceAmount,
						FMath::Max(RefillDurationSteps, 1)),
					1));
		}
		constexpr uint16 MaterialIndexMask = 0x7fffu;

		uint16 MergeSpecificEnergy(
			const uint16 ExistingAmount,
			const uint16 ExistingEnergy,
			const uint16 AddedAmount,
			const uint16 AddedEnergy)
		{
			const uint32 TotalAmount =
				static_cast<uint32>(ExistingAmount) + AddedAmount;
			if (TotalAmount == 0)
			{
				return 0;
			}
			const int64 TotalEnergy =
				static_cast<int64>(ExistingAmount) * ExistingEnergy
				+ static_cast<int64>(AddedAmount) * AddedEnergy;
			return static_cast<uint16>(TotalEnergy / TotalAmount);
		}

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

		int64 GetPowderMaximumStableSlope255(
			const FWorldSettings& Settings,
			const uint32 Seed,
			const FIntPoint& A,
			const FIntPoint& B)
		{
			// The authored repose value is the allowed height change across one
			// orthogonal cell. A diagonal edge is sqrt(2) times longer, so applying
			// the same threshold there makes powder leak unrealistically through the
			// corners and produces overly regular rings.
			int64 MaximumSlope255 =
				static_cast<int64>(PowderFullColumnHeight)
					* Settings.PowderMaximumStableSlopeAmount;
			if (A.X != B.X && A.Y != B.Y)
			{
				MaximumSlope255 = (MaximumSlope255 * 1414 + 500) / 1000;
			}

			// Real granular piles do not have an identical repose threshold at
			// every grain contact. Give each undirected cell edge a small, stable
			// variation. It is seeded and independent of Tick, so a settled pile
			// remains settled and deterministic instead of visibly crawling.
			FIntPoint First = A;
			FIntPoint Second = B;
			if (Second.X < First.X
				|| (Second.X == First.X && Second.Y < First.Y))
			{
				Swap(First, Second);
			}
			const uint32 EdgeHash = MixBits(
				Seed
					^ static_cast<uint32>(First.X) * 0x9e3779b9u
					^ static_cast<uint32>(First.Y) * 0x85ebca6bu
					^ static_cast<uint32>(Second.X) * 0xc2b2ae35u
					^ static_cast<uint32>(Second.Y) * 0x27d4eb2fu);
			constexpr int32 ReposeVariationPercent = 30;
			const int32 VariationPercent =
				static_cast<int32>(EdgeHash % (ReposeVariationPercent * 2 + 1))
					- ReposeVariationPercent;
			return FMath::Max<int64>(
				1,
				MaximumSlope255 * (100 + VariationPercent) / 100);
		}

		int64 GetPowderPackingOffset255(
			const uint32 Seed,
			const FIntPoint& Cell)
		{
			// Coarse surface cells stand for many grains. Their local packing density
			// is not identical, so give each cell a stable sub-layer height offset.
			// This changes canonical flow equilibrium (and therefore occupancy), not
			// only the mesh. The hash is independent of Tick: settled sand stays still.
			const uint32 PackingHash = MixBits(
				Seed ^ static_cast<uint32>(Cell.X) * 0x9e3779b9u
					^ static_cast<uint32>(Cell.Y) * 0x85ebca6bu
					^ 0x68bc21ebu);
			constexpr int64 MaximumOffset255 =
				static_cast<int64>(PowderFullColumnHeight)
					* FullCellAmount * 40 / 100;
			return static_cast<int64>(
				PackingHash % static_cast<uint32>(MaximumOffset255 * 2 + 1))
				- MaximumOffset255;
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
			uint16 DefaultEnergy = 0;
			uint16 ConductivityPermille = 0;
			uint16 CoolingPerStep = 0;
			uint16 IgnitionThreshold = 0;
			uint16 IgnitionProductMaterialIndex = 0;
			uint16 IgnitionEmissionMaterialIndex = 0;
			uint16 IgnitionEmissionAmount = 0;
			uint16 IgnitionSecondaryEmissionMaterialIndex = 0;
			uint16 IgnitionSecondaryEmissionAmount = 0;
		};

		struct FCell
		{
			struct FLayer
			{
				uint16 MaterialIndex = 0;
				uint16 Amount = 0;
				uint8 RemainingLifetime = 0;
				uint16 Energy = 0;
				uint32 LastUpdatedTick = 0;
			};

			uint16 MaterialIndex = 0;
			int16 SupportHeight = 0;
			uint16 Amount = 0;
			uint8 RemainingLifetime = 0;
			uint16 Energy = 0;
			uint32 LastUpdatedTick = 0;
			/** Dense bottom-to-top layers below the primary/top material. */
			TArray<FLayer> Underlayers;
			// A transient body displaced volume from this world column. This is
			// location state, not material state: it must not travel with liquid.
			// The hydraulic solver uses it briefly to refill across uneven terrain
			// without globally connecting authored rivers and lakes by elevation.
			uint32 BodyDisplacedTick = 0;
			uint16 BodyDisplacedMaterialIndex = 0;
			uint8 BodyDisplacedReferenceAmount = 0;
			bool bHasBodyDisplacementVacancy = false;
		};

		struct FChunk
		{
			explicit FChunk(const int32 InChunkSize)
				: ChunkSize(InChunkSize)
			{
				Cells.SetNum(ChunkSize * ChunkSize);
				DirtyCellFlags.Init(false, Cells.Num());
			}

			TArray<FCell> Cells;
			int32 ChunkSize = 0;
			TBitArray<> DirtyCellFlags;
			TArray<int32> DirtyCellIndices;

			void MarkDirty(const FIntPoint& LocalCell)
			{
				const int32 CellIndex = LocalCell.Y * ChunkSize + LocalCell.X;
				if (!DirtyCellFlags.IsValidIndex(CellIndex)
					|| DirtyCellFlags[CellIndex])
				{
					return;
				}
				DirtyCellFlags[CellIndex] = true;
				DirtyCellIndices.Add(CellIndex);
			}

			void MarkAllDirty()
			{
				DirtyCellFlags.Init(true, Cells.Num());
				DirtyCellIndices.SetNumUninitialized(Cells.Num());
				for (int32 CellIndex = 0; CellIndex < Cells.Num(); ++CellIndex)
				{
					DirtyCellIndices[CellIndex] = CellIndex;
				}
			}

			void ResetDirty()
			{
				for (const int32 CellIndex : DirtyCellIndices)
				{
					DirtyCellFlags[CellIndex] = false;
				}
				DirtyCellIndices.Reset();
			}

			bool HasDirtyCells() const
			{
				return !DirtyCellIndices.IsEmpty();
			}
		};

		struct FArchivedRun
		{
			uint16 MaterialIndex = 0;
			int16 SupportHeight = 0;
			uint16 Amount = 0;
			uint16 Energy = 0;
			uint16 Length = 0;
		};

		struct FArchivedChunk
		{
			TArray<FArchivedRun> Runs;
			TMap<uint16, TArray<FCell::FLayer>> UnderlayersByCell;
		};

		FWorldSettings Settings;
		TArray<FResolvedMaterial> Materials;
		TMap<FName, uint16> MaterialIndices;
		FLocalMaterialReactionProgram LocalReactionProgram;
		TArray<FReactionEmission> PendingReactionEmissions;
		int32 LocalReactionRevision = 0;
		TMap<FIntPoint, TUniquePtr<FChunk>> Chunks;
		TMap<FIntPoint, FArchivedChunk> ArchivedChunks;
		/** Deterministic generated-map seed used as the replication baseline. */
		TMap<FIntPoint, FArchivedChunk> BaselineChunks;
		/** Disposable collision supports supplied by world objects; never saved. */
		TMap<FIntPoint, int16> ExternalSupportHeights;
		/** Sparse transient columns whose liquid volume was constrained by bodies. */
		TSet<FIntPoint> BodyDisplacementVacancies;
		/** Chunks whose canonical facts changed since the last render projection. */
		TSet<FIntPoint> ProjectionDirtyChunks;
		FLiquidDisplacementStats LastLiquidDisplacementStats;
		FIntPoint FocusCell = FIntPoint::ZeroValue;
		TArray<FIntPoint> FocusCells;
		TSet<FIntPoint> ActiveChunks;
		/**
		 * Surface chunks temporarily kept resident while liquid or powder settles
		 * beyond the player-focused simulation window. They retire as soon as their
		 * dirty work settles, so a streaming seam can never act as physical terrain.
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
			uint16 CurrentEnergy = Chunk.Cells[0].Energy;
			uint16 CurrentLength = 0;
			for (int32 CellIndex = 0; CellIndex < Chunk.Cells.Num(); ++CellIndex)
			{
				const FCell& Cell = Chunk.Cells[CellIndex];
				if (!Cell.Underlayers.IsEmpty())
				{
					Archive.UnderlayersByCell.Add(
						static_cast<uint16>(CellIndex), Cell.Underlayers);
				}
				if (Cell.MaterialIndex != CurrentMaterial
					|| Cell.SupportHeight != CurrentSupport
					|| Cell.Amount != CurrentAmount
					|| Cell.Energy != CurrentEnergy
					|| CurrentLength == MAX_uint16)
				{
					Archive.Runs.Add({
						CurrentMaterial,
						CurrentSupport,
						CurrentAmount,
						CurrentEnergy,
						CurrentLength });
					CurrentMaterial = Cell.MaterialIndex;
					CurrentSupport = Cell.SupportHeight;
					CurrentAmount = Cell.Amount;
					CurrentEnergy = Cell.Energy;
					CurrentLength = 0;
				}
				++CurrentLength;
			}
			Archive.Runs.Add({
				CurrentMaterial,
				CurrentSupport,
				CurrentAmount,
				CurrentEnergy,
				CurrentLength });
			return Archive;
		}

		bool HasPersistentData(const FChunk& Chunk) const
		{
			return Chunk.Cells.ContainsByPredicate([](const FCell& Cell)
			{
				return Cell.MaterialIndex != 0
					|| !Cell.Underlayers.IsEmpty()
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
					OutChunk.Cells[CellIndex].Energy = Run.Energy;
					OutChunk.Cells[CellIndex].RemainingLifetime = 0;
					OutChunk.Cells[CellIndex].LastUpdatedTick = 0;
					OutChunk.Cells[CellIndex].BodyDisplacedTick = 0;
					OutChunk.Cells[CellIndex].BodyDisplacedMaterialIndex = 0;
					OutChunk.Cells[CellIndex].BodyDisplacedReferenceAmount = 0;
					OutChunk.Cells[CellIndex].bHasBodyDisplacementVacancy = false;
					++CellIndex;
				}
			}
			for (const TPair<uint16, TArray<FCell::FLayer>>& Pair
				: Archive.UnderlayersByCell)
			{
				if (OutChunk.Cells.IsValidIndex(Pair.Key))
				{
					OutChunk.Cells[Pair.Key].Underlayers = Pair.Value;
					for (FCell::FLayer& Layer
						: OutChunk.Cells[Pair.Key].Underlayers)
					{
						Layer.LastUpdatedTick = 0;
					}
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
				const int32 LayerCountBefore = Cell.Underlayers.Num();
				Cell.Underlayers.RemoveAllSwap([](const FCell::FLayer& Layer)
				{
					return Layer.RemainingLifetime != 0;
				}, EAllowShrinking::No);
				bDiscardedTransientMaterial |=
					Cell.Underlayers.Num() != LayerCountBefore;
				if (Cell.RemainingLifetime == 0)
				{
					continue;
				}
				Cell.MaterialIndex = 0;
				Cell.Amount = 0;
				Cell.Energy = 0;
				Cell.RemainingLifetime = 0;
				PromoteTopUnderlayer(Cell);
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

		void TrimTransientSurfaceFlowChunks()
		{
			if (!Settings.bUseSurfaceTopology || SurfaceFlowChunks.IsEmpty())
			{
				return;
			}
			// Active chunks already participate in the normal fixed-step window and
			// need no second residency pin. Keeping them in both sets made every
			// traversed river chunk survive after the focus moved on.
			for (const FIntPoint ActiveChunk : ActiveChunks)
			{
				SurfaceFlowChunks.Remove(ActiveChunk);
			}
			const int32 MaximumTransientFlowChunks = FMath::Max(
				1, Settings.MaxActiveChunks / 3);
			if (SurfaceFlowChunks.Num() <= MaximumTransientFlowChunks)
			{
				return;
			}

			struct FFlowChunkDistance
			{
				FIntPoint Chunk = FIntPoint::ZeroValue;
				int64 Distance = MAX_int64;
			};
			TArray<FFlowChunkDistance> Ordered;
			Ordered.Reserve(SurfaceFlowChunks.Num());
			for (const FIntPoint ChunkCoordinate : SurfaceFlowChunks)
			{
				int64 NearestDistance = MAX_int64;
				for (const FIntPoint Focus : FocusCells)
				{
					const FIntPoint FocusChunk = ToChunkCoordinate(Focus);
					NearestDistance = FMath::Min(
						NearestDistance,
						FMath::Abs(static_cast<int64>(ChunkCoordinate.X)
							- FocusChunk.X)
							+ FMath::Abs(static_cast<int64>(ChunkCoordinate.Y)
								- FocusChunk.Y));
				}
				Ordered.Add({ ChunkCoordinate, NearestDistance });
			}
			Ordered.Sort([](const FFlowChunkDistance& A,
				const FFlowChunkDistance& B)
			{
				if (A.Distance != B.Distance) return A.Distance < B.Distance;
				return A.Chunk.X != B.Chunk.X
					? A.Chunk.X < B.Chunk.X
					: A.Chunk.Y < B.Chunk.Y;
			});
			for (int32 Index = MaximumTransientFlowChunks;
				Index < Ordered.Num(); ++Index)
			{
				SurfaceFlowChunks.Remove(Ordered[Index].Chunk);
				ArchiveChunk(Ordered[Index].Chunk);
			}
		}

		FChunk& FindOrAddChunk(const FIntPoint& ChunkCoordinate)
		{
			TUniquePtr<FChunk>& Chunk = Chunks.FindOrAdd(ChunkCoordinate);
			if (!Chunk)
			{
				Chunk = MakeUnique<FChunk>(Settings.ChunkSize);
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

		uint16 FindArchivedEnergy(const FIntPoint& WorldCell) const
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
					return Run.Energy;
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

		bool FindBaseSupportHeight(
			const FIntPoint& WorldCell,
			int16& OutSupportHeight) const
		{
			const int16* ExternalSupport =
				ExternalSupportHeights.Find(WorldCell);
			if (const FCell* Cell = FindCell(WorldCell))
			{
				OutSupportHeight = ExternalSupport
					? FMath::Max(Cell->SupportHeight, *ExternalSupport)
					: Cell->SupportHeight;
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
					OutSupportHeight = ExternalSupport
						? FMath::Max(Run.SupportHeight, *ExternalSupport)
						: Run.SupportHeight;
					return true;
				}
				RemainingIndex -= Run.Length;
			}
			return false;
		}

		int32 GetLayerHeight(
			const uint16 MaterialIndex,
			const uint16 Amount) const
		{
			if (MaterialIndex == 0 || MaterialIndex >= Materials.Num())
			{
				return 0;
			}
			switch (Materials[MaterialIndex].Phase)
			{
			case EMatterFluxMaterialPhase::Powder:
				return FMath::Max(1, FMath::RoundToInt(
					static_cast<double>(PowderFullColumnHeight) * Amount
						/ FullCellAmount));
			case EMatterFluxMaterialPhase::Liquid:
				return FMath::Max(1, FMath::RoundToInt(
					static_cast<double>(Settings.LiquidFullColumnHeight)
						* FMath::Min<uint16>(Amount, FullCellAmount)
						/ FullCellAmount));
			default:
				return 0;
			}
		}

		int32 GetUnderlayerHeight(
			TConstArrayView<FCell::FLayer> Underlayers) const
		{
			int32 Height = 0;
			for (const FCell::FLayer& Layer : Underlayers)
			{
				Height += GetLayerHeight(Layer.MaterialIndex, Layer.Amount);
			}
			return Height;
		}

		bool FindSupportHeight(
			const FIntPoint& WorldCell,
			int16& OutSupportHeight) const
		{
			if (!FindBaseSupportHeight(WorldCell, OutSupportHeight))
			{
				return false;
			}
			int32 Height = OutSupportHeight;
			if (const FCell* Cell = FindCell(WorldCell))
			{
				Height += GetUnderlayerHeight(Cell->Underlayers);
			}
			else if (const FArchivedChunk* Archive =
				ArchivedChunks.Find(ToChunkCoordinate(WorldCell)))
			{
				const uint16 CellIndex = static_cast<uint16>(
					ToIndex(ToLocalCoordinate(WorldCell)));
				if (const TArray<FCell::FLayer>* Layers =
					Archive->UnderlayersByCell.Find(CellIndex))
				{
					Height += GetUnderlayerHeight(*Layers);
				}
			}
			OutSupportHeight = static_cast<int16>(FMath::Clamp(
				Height,
				static_cast<int32>(MIN_int16),
				static_cast<int32>(MAX_int16)));
			return true;
		}

		void PromoteTopUnderlayer(FCell& Cell)
		{
			if (Cell.MaterialIndex != 0
				&& Cell.Amount != 0)
			{
				return;
			}
			if (Cell.Underlayers.IsEmpty())
			{
				Cell.MaterialIndex = 0;
				Cell.Amount = 0;
				Cell.RemainingLifetime = 0;
				Cell.Energy = 0;
				return;
			}
			const FCell::FLayer Top = Cell.Underlayers.Pop(EAllowShrinking::No);
			Cell.MaterialIndex = Top.MaterialIndex;
			Cell.Amount = Top.Amount;
			Cell.RemainingLifetime = Top.RemainingLifetime;
			Cell.Energy = Top.Energy;
			Cell.LastUpdatedTick = Top.LastUpdatedTick;
		}

		void QueueReactionEmissions(
			const FMaterialDeltaBatch& Batch,
			const FIntVector& FallbackGridCell)
		{
			for (const FMaterialParticleEmission& Emission
				: Batch.ParticleEmissions)
			{
				check(Materials.IsValidIndex(Emission.MaterialIndex));
				FReactionEmission& Pending =
					PendingReactionEmissions.AddDefaulted_GetRef();
				Pending.ParticleId = Emission.ParticleId;
				Pending.MaterialId = Materials[Emission.MaterialIndex].Id;
				Pending.GridCell = Emission.Emitter.Kind
					== EMaterialElementAddressKind::WorldCell
					? Emission.Emitter.Cell
					: FallbackGridCell;
				Pending.Amount = Emission.Amount;
				Pending.Energy = Emission.Energy;
				Pending.RemainingLifetimeSteps =
					Materials[Emission.MaterialIndex].LifetimeSteps;
			}
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
			return LocalReactionProgram.GetContactRules().ContainsByPredicate(
				[FirstMaterialIndex, SecondMaterialIndex](
					const FLocalMaterialContactRule& Reaction)
				{
					return (Reaction.InputA == FirstMaterialIndex
							&& Reaction.InputB == SecondMaterialIndex)
						|| (Reaction.InputA == SecondMaterialIndex
							&& Reaction.InputB == FirstMaterialIndex);
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
			Chunk.ResetDirty();
			if (BaselineChunks.IsEmpty())
			{
				Chunk.MarkAllDirty();
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

		void MarkChunkFlowMaterialsDirty(
			const FIntPoint& ChunkCoordinate)
		{
			FChunk& Chunk = FindOrAddChunk(ChunkCoordinate);
			TArray<FIntPoint> FlowCells;
			for (int32 CellIndex = 0; CellIndex < Chunk.Cells.Num(); ++CellIndex)
			{
				const FCell& Cell = Chunk.Cells[CellIndex];
				if (Cell.MaterialIndex == 0
					|| Cell.MaterialIndex >= Materials.Num())
				{
					continue;
				}
				const EMatterFluxMaterialPhase Phase =
					Materials[Cell.MaterialIndex].Phase;
				if (Phase != EMatterFluxMaterialPhase::Liquid
					&& Phase != EMatterFluxMaterialPhase::Powder
					&& Phase != EMatterFluxMaterialPhase::Gas)
				{
					continue;
				}
				FlowCells.Add(FIntPoint(
					ChunkCoordinate.X * Settings.ChunkSize
						+ CellIndex % Settings.ChunkSize,
					ChunkCoordinate.Y * Settings.ChunkSize
						+ CellIndex / Settings.ChunkSize));
			}
			for (const FIntPoint FlowCell : FlowCells)
			{
				MarkDirtyNeighborhood(FlowCell);
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

		bool TryMoveSurfaceUnderlayerPowder(
			const FIntPoint& Source,
			FStepStats& Stats)
		{
			FCell* SourceCell = FindCell(Source);
			if (!SourceCell || SourceCell->Underlayers.IsEmpty())
			{
				return false;
			}
			int32 SourceLayerIndex = INDEX_NONE;
			for (int32 Index = 0; Index < SourceCell->Underlayers.Num(); ++Index)
			{
				const uint16 IndexMaterial =
					SourceCell->Underlayers[Index].MaterialIndex;
				if (IndexMaterial < Materials.Num()
					&& Materials[IndexMaterial].Phase
						== EMatterFluxMaterialPhase::Powder)
				{
					SourceLayerIndex = Index;
					break;
				}
			}
			if (SourceLayerIndex == INDEX_NONE)
			{
				return false;
			}
			const FCell::FLayer SourceLayer =
				SourceCell->Underlayers[SourceLayerIndex];
			const FResolvedMaterial& Powder = Materials[SourceLayer.MaterialIndex];
			if (!PassesMobilityCheck(Source, Powder))
			{
				return false;
			}

			int16 SourceSupport = 0;
			if (!FindBaseSupportHeight(Source, SourceSupport))
			{
				return false;
			}
			int32 SourceBase = SourceSupport;
			for (int32 Index = 0; Index < SourceLayerIndex; ++Index)
			{
				const FCell::FLayer& Layer = SourceCell->Underlayers[Index];
				SourceBase += GetLayerHeight(Layer.MaterialIndex, Layer.Amount);
			}
			const int64 SourceSurface255 =
				static_cast<int64>(SourceBase) * FullCellAmount
					+ static_cast<int64>(PowderFullColumnHeight)
						* SourceLayer.Amount
					+ GetPowderPackingOffset255(Seed, Source);
			static const FIntPoint Directions[] = {
				FIntPoint(1, 0), FIntPoint(0, 1), FIntPoint(-1, 0),
				FIntPoint(0, -1), FIntPoint(1, 1), FIntPoint(-1, 1),
				FIntPoint(-1, -1), FIntPoint(1, -1) };
			const uint32 DirectionHash = MixBits(
				Seed ^ static_cast<uint32>(Source.X) * 0x9e3779b9u
					^ static_cast<uint32>(Source.Y) * 0x85ebca6bu ^ Tick);
			const int32 FirstDirection =
				static_cast<int32>(DirectionHash % UE_ARRAY_COUNT(Directions));

			FIntPoint BestDestination = Source;
			int64 BestExcessSurface255 = 0;
			int64 BestFlowScore = 0;
			uint16 DestinationAmount = 0;
			for (int32 Offset = 0; Offset < UE_ARRAY_COUNT(Directions); ++Offset)
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
				if (!FindBaseSupportHeight(Destination, DestinationSupport))
				{
					continue;
				}
				const FCell* DestinationCell = FindCell(Destination);
				if (!DestinationCell || DestinationCell->MaterialIndex == 0
					|| DestinationCell->MaterialIndex >= Materials.Num()
					|| Materials[DestinationCell->MaterialIndex].Phase
						== EMatterFluxMaterialPhase::StaticSolid
					|| Materials[DestinationCell->MaterialIndex].Density
						>= Powder.Density)
				{
					continue;
				}
				int32 DestinationBase = DestinationSupport;
				uint16 ExistingPowderAmount = 0;
				for (const FCell::FLayer& Layer : DestinationCell->Underlayers)
				{
					if (Layer.MaterialIndex == SourceLayer.MaterialIndex)
					{
						ExistingPowderAmount = Layer.Amount;
						break;
					}
					if (Layer.MaterialIndex < Materials.Num()
						&& Materials[Layer.MaterialIndex].Density >= Powder.Density)
					{
						DestinationBase += GetLayerHeight(
							Layer.MaterialIndex, Layer.Amount);
					}
				}
				const int64 CandidateSurface255 =
					static_cast<int64>(DestinationBase) * FullCellAmount
						+ static_cast<int64>(PowderFullColumnHeight)
							* ExistingPowderAmount
						+ GetPowderPackingOffset255(Seed, Destination);
				// A lower neighboring support is a spill drop, not a continuous
				// downhill plane through empty air. Compare the granular surface at
				// least against this layer's source-side lip. Material above the lip
				// can fall over it; a supported coating below the repose threshold
				// remains on the platform. This is generic height-field geometry for
				// cliffs, roofs, leaves, and furniture rather than an object rule.
				const int64 SpillSurface255 = FMath::Max(
					CandidateSurface255,
					static_cast<int64>(SourceBase) * FullCellAmount);
				const int64 ExcessSurface255 =
					SourceSurface255 - SpillSurface255
						- GetPowderMaximumStableSlope255(
							Settings, Seed, Source, Destination);
				const uint32 FlowHash = MixBits(
					Seed ^ Tick * 0x27d4eb2fu
						^ static_cast<uint32>(Source.X) * 0x9e3779b9u
						^ static_cast<uint32>(Source.Y) * 0x85ebca6bu
						^ static_cast<uint32>(Destination.X) * 0xc2b2ae35u
						^ static_cast<uint32>(Destination.Y) * 0x165667b1u);
				const int64 FlowScore = ExcessSurface255 > 0
					? ExcessSurface255 * (70 + FlowHash % 61)
					: 0;
				if (FlowScore > BestFlowScore)
				{
					BestFlowScore = FlowScore;
					BestExcessSurface255 = ExcessSurface255;
					BestDestination = Destination;
					DestinationAmount = ExistingPowderAmount;
				}
			}
			if (BestDestination == Source || BestExcessSurface255 <= 0)
			{
				return false;
			}
			const uint32 TransferHash = MixBits(
				Seed ^ Tick * 0x9e3779b9u
					^ static_cast<uint32>(Source.X) * 0x85ebca6bu
					^ static_cast<uint32>(Source.Y) * 0xc2b2ae35u
					^ static_cast<uint32>(BestDestination.X) * 0x27d4eb2fu
					^ static_cast<uint32>(BestDestination.Y) * 0x165667b1u);
			const int32 EqualizingTransfer = FMath::Max<int32>(
				static_cast<int32>(
					BestExcessSurface255 / (2 * PowderFullColumnHeight)), 1);
			const int32 RequestedTransfer = FMath::Max(
				EqualizingTransfer * (55 + static_cast<int32>(TransferHash % 46))
					/ 100,
				1);
			const int32 MovableAmount = SourceLayer.Amount;
			const int32 Transfer = FMath::Min(
				FMath::Min3(
					RequestedTransfer,
					MovableAmount,
					static_cast<int32>(MAX_uint16 - DestinationAmount)),
				static_cast<int32>(FullCellAmount));
			if (Transfer <= 0)
			{
				return false;
			}

			WakeSurfaceFlow(BestDestination);
			FCell& MutableDestination = FindOrAddCell(BestDestination);
			FCell::FLayer* DestinationLayer =
				MutableDestination.Underlayers.FindByPredicate(
					[&SourceLayer](const FCell::FLayer& Layer)
					{
						return Layer.MaterialIndex == SourceLayer.MaterialIndex;
					});
			if (!DestinationLayer)
			{
				DestinationLayer = &MutableDestination.Underlayers.AddDefaulted_GetRef();
				DestinationLayer->MaterialIndex = SourceLayer.MaterialIndex;
				DestinationLayer->RemainingLifetime = SourceLayer.RemainingLifetime;
				MutableDestination.Underlayers.Sort([this](
					const FCell::FLayer& A, const FCell::FLayer& B)
				{
					return Materials[A.MaterialIndex].Density
						> Materials[B.MaterialIndex].Density;
				});
				DestinationLayer = MutableDestination.Underlayers.FindByPredicate(
					[&SourceLayer](const FCell::FLayer& Layer)
					{
						return Layer.MaterialIndex == SourceLayer.MaterialIndex;
					});
			}
			DestinationLayer->Energy = MergeSpecificEnergy(
				DestinationLayer->Amount, DestinationLayer->Energy,
				static_cast<uint16>(Transfer), SourceLayer.Energy);
			DestinationLayer->Amount = static_cast<uint16>(
				DestinationLayer->Amount + Transfer);
			DestinationLayer->LastUpdatedTick = Tick;
			SourceCell = FindCell(Source);
			FCell::FLayer& MutableSourceLayer =
				SourceCell->Underlayers[SourceLayerIndex];
			MutableSourceLayer.Amount = static_cast<uint16>(
				MutableSourceLayer.Amount - Transfer);
			MutableSourceLayer.LastUpdatedTick = Tick;
			if (MutableSourceLayer.Amount == 0)
			{
				SourceCell->Underlayers.RemoveAt(
					SourceLayerIndex, 1, EAllowShrinking::No);
			}
			MarkDirtyNeighborhood(Source);
			MarkDirtyNeighborhood(BestDestination);
			++Stats.MovedCells;
			return true;
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
							* SourceCell->Amount
						+ GetPowderPackingOffset255(Seed, Source);
				const int16* ExternalSourceSupport =
					ExternalSupportHeights.Find(Source);
				const bool bOnElevatedExternalSupport = ExternalSourceSupport
					&& *ExternalSourceSupport > SourceCell->SupportHeight;
				FIntPoint LowestDestination = Source;
				int64 GreatestExcessSurface255 = 0;
				int64 GreatestFlowScore = 0;
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
								* DestinationAmount
							+ GetPowderPackingOffset255(Seed, Destination);
					// Do not interpret a vertical support discontinuity as an
					// infinitely steep terrain ramp. The source support is the spill
					// lip: excess powder crosses it and falls, while a repose-stable
					// layer with full support remains on the upper platform.
					// Terrain ledges retain a repose-stable layer at their spill lip.
					// An isolated object support is different: once grains cross its
					// footprint they fall to the actual destination surface. Clamping
					// that fall to the source support left a permanent hovering layer.
					const int64 SpillSurface255 = bOnElevatedExternalSupport
						? DestinationSurface255
						: FMath::Max(
							DestinationSurface255,
							static_cast<int64>(SourceSupport) * FullCellAmount);
					const int64 ExcessSurface255 =
						SourceSurface255 - SpillSurface255
							- GetPowderMaximumStableSlope255(
								Settings, Seed, Source, Destination);
					const uint32 FlowHash = MixBits(
						Seed ^ Tick * 0x27d4eb2fu
							^ static_cast<uint32>(Source.X) * 0x9e3779b9u
							^ static_cast<uint32>(Source.Y) * 0x85ebca6bu
							^ static_cast<uint32>(Destination.X) * 0xc2b2ae35u
							^ static_cast<uint32>(Destination.Y) * 0x165667b1u);
					const int64 FlowScore = ExcessSurface255 > 0
						? ExcessSurface255 * (70 + FlowHash % 61)
						: 0;
					if (FlowScore > GreatestFlowScore)
					{
						GreatestFlowScore = FlowScore;
						GreatestExcessSurface255 = ExcessSurface255;
						LowestDestination = Destination;
						LowestAmount = DestinationAmount;
					}
				}

				if (LowestDestination == Source || GreatestExcessSurface255 <= 0)
				{
					return false;
				}

				const int32 EqualizingTransfer = FMath::Max<int32>(
					static_cast<int32>(
						GreatestExcessSurface255
							/ (2 * PowderFullColumnHeight)),
					1);
				const uint32 TransferHash = MixBits(
					Seed ^ Tick * 0x9e3779b9u
						^ static_cast<uint32>(Source.X) * 0x85ebca6bu
						^ static_cast<uint32>(Source.Y) * 0xc2b2ae35u
						^ static_cast<uint32>(LowestDestination.X) * 0x27d4eb2fu
						^ static_cast<uint32>(LowestDestination.Y) * 0x165667b1u);
				const int32 RequestedTransfer = FMath::Max(
					EqualizingTransfer
						* (55 + static_cast<int32>(TransferHash % 46)) / 100,
					1);
				const int32 MovableAmount = SourceCell->Amount;
				const int32 TransferAmount = FMath::Min(
					FMath::Min3(
						RequestedTransfer,
						MovableAmount,
						static_cast<int32>(MAX_uint16 - LowestAmount)),
					static_cast<int32>(FullCellAmount));
				if (TransferAmount <= 0)
				{
					return false;
				}

				WakeSurfaceFlow(LowestDestination);
				FCell& DestinationCell = FindOrAddCell(LowestDestination);
				SourceCell = FindCell(Source);
				const uint16 DestinationAmountBefore = DestinationCell.Amount;
				DestinationCell.MaterialIndex = SourceCell->MaterialIndex;
				DestinationCell.Energy = MergeSpecificEnergy(
					DestinationAmountBefore, DestinationCell.Energy,
					static_cast<uint16>(TransferAmount), SourceCell->Energy);
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
					SourceCell->Energy = 0;
					PromoteTopUnderlayer(*SourceCell);
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
			bool bHasReachableLowerNeighbor = false;
			for (const FIntPoint Direction : Directions)
			{
				FIntPoint Neighbor;
				if (!TryOffsetCell(Source, Direction, Neighbor)
					|| (Settings.bCullOutsideSurfaceBounds
						&& !IsInsideSurfaceBounds(Neighbor)))
				{
					continue;
				}
				int16 NeighborSupport = 0;
				if (!FindSupportHeight(Neighbor, NeighborSupport)
					|| NeighborSupport >= SourceSupport)
				{
					continue;
				}
				const FCell* NeighborCell = FindCell(Neighbor);
				bHasReachableLowerNeighbor = !NeighborCell
					|| NeighborCell->MaterialIndex == 0
					|| (NeighborCell->MaterialIndex == SourceCell->MaterialIndex
						&& NeighborCell->Amount < FullCellAmount);
				if (bHasReachableLowerNeighbor)
				{
					break;
				}
			}
			// Round-puddle shaping is a level-ground presentation preference.
			// Gravity must own the decision wherever a lower reachable column
			// exists, otherwise shallow spell deposits are merged back into their
			// authored circle before the hydraulic solver can move them downhill.
			const FSurfacePuddleShape* EffectivePuddleShape =
				bHasReachableLowerNeighbor ? nullptr : PuddleShape;
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
			const int8 AxisBias = EffectivePuddleShape
				? EffectivePuddleShape->AxisBias
				: 0;
			const auto RadialDistance = [EffectivePuddleShape](const FIntPoint& Cell)
			{
				if (!EffectivePuddleShape)
				{
					return 0.0f;
				}
				const FVector2D Delta(
					static_cast<double>(Cell.X) - EffectivePuddleShape->Center.X,
					static_cast<double>(Cell.Y) - EffectivePuddleShape->Center.Y);
				return static_cast<float>(Delta.Length());
			};

			// 小型水洼以液量推导目标圆盘。超出圆盘的低量边缘优先并回
			// 更靠近质心的同材质格，这一步只做局部转移并严格守恒。
			if (EffectivePuddleShape)
			{
				const float SourceRadius = RadialDistance(Source);
				if (SourceRadius > EffectivePuddleShape->Radius + 0.35f
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
						DestinationCell.Energy = MergeSpecificEnergy(
							DestinationCell.Amount, DestinationCell.Energy,
							static_cast<uint16>(TransferAmount),
							MutableSourceCell.Energy);
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
							MutableSourceCell.Energy = 0;
							PromoteTopUnderlayer(MutableSourceCell);
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
			const int32 LiquidEdgeTransferAmount =
				GetLiquidEdgeTransferAmount(Material.Dispersion);
			if (!bHasReachableLowerNeighbor
				&& MinimumRetainedAmount == 0
				&& SourceCell->Amount <= LiquidEdgeTransferAmount + 4)
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
					NeighborCell.Energy = MergeSpecificEnergy(
						NeighborCell.Amount, NeighborCell.Energy,
						MutableSourceCell.Amount, MutableSourceCell.Energy);
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
					MutableSourceCell.Energy = 0;
					PromoteTopUnderlayer(MutableSourceCell);
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
			int16 LateralDestinationSupport = SourceSupport;
			const int64 SourceSurface255 =
				static_cast<int64>(SourceSupport) * FullCellAmount
					+ static_cast<int64>(Settings.LiquidFullColumnHeight)
						* SourceCell->Amount;
			const float CohesionScale =
				(255.0f - static_cast<float>(Material.Dispersion)) / 255.0f;
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
				const bool bBodyWakeWaitingForRefill = bRecentBodyVacancy
					&& DestinationCell->BodyDisplacedTick <= Tick
					&& Tick - DestinationCell->BodyDisplacedTick
						<= static_cast<uint32>(
							Settings.BodyWakeRefillDelaySteps);
				// During the authored hold interval the vacancy is still a material
				// transaction fact. Ordinary lateral pressure may not bypass it and
				// start the wake before the restitution clock permits.
				if (bBodyWakeWaitingForRefill)
				{
					continue;
				}
				// Restitution already spent this wake column's bounded transfer
				// budget for the fixed step. Do not let the ordinary lateral solver
				// add another transfer and collapse a visible trail in one update.
				if (bRecentBodyVacancy
					&& DestinationCell->LastUpdatedTick == Tick)
				{
					continue;
				}
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
					// Interior preference is surface cohesion, not a wall. Keeping this
					// at a fixed -128 made water repeatedly choose a slightly lower
					// occupied neighbor over a dry, much lower shoreline coordinate,
					// freezing a continuous spray into a one-sided 5x4 block.
					const float InteriorPreference = FMath::Lerp(
						128.0f,
						128.0f * CohesionScale,
						GetLiquidFreeFlowBlend(Material.Dispersion));
					DirectionPenalty -= FMath::RoundToInt(InteriorPreference);
				}
				if (DestinationAmount == 0)
				{
					const FIntPoint Direction = Directions[DirectionIndex];
					if (EffectivePuddleShape)
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
							EffectivePuddleShape->Radius + BoundaryJitter;
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
					LateralDestinationSupport = DestinationSupport;
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
					? FMath::Max(FMath::RoundToInt(8.0f * CohesionScale), 0)
					: FMath::Max(FMath::RoundToInt(
						(84.0f + static_cast<float>(EdgeHash % 37u))
							* CohesionScale), 0))
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
				const int32 AxisPenalty = FMath::RoundToInt(
					32.0f * CohesionScale);
				SurfaceTension = bAlongLongAxis
					? SurfaceTension + AxisPenalty
					: (bAlongShortAxis
						? FMath::Max(
							SurfaceTension - AxisPenalty,
							LiquidEdgeTransferAmount)
						: SurfaceTension);
			}
			if (LowestAmount == 0 && EffectivePuddleShape)
			{
				const float DestinationRadius =
					RadialDistance(LateralDestination);
				const int32 BoundaryPenalty = FMath::Max(
					FMath::RoundToInt(56.0f * CohesionScale),
					LiquidEdgeTransferAmount);
				SurfaceTension = DestinationRadius <= EffectivePuddleShape->Radius
					? FMath::Max(
						SurfaceTension - LiquidEdgeTransferAmount,
						0)
					: SurfaceTension + BoundaryPenalty;
			}
			const bool bDownhill =
				LateralDestinationSupport < SourceSupport;
			if (bDownhill)
			{
				// Surface tension may shape a level shoreline, but it cannot
				// cancel gravity. This also lets very shallow projectile liquid
				// cross a dry edge in several small, conserved transfers.
				SurfaceTension = 0;
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
			if (!bDownhill
				&& LowestAmount == 0
				&& TransferAmount < LiquidEdgeTransferAmount)
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
			DestinationCell.Energy = MergeSpecificEnergy(
				DestinationCell.Amount, DestinationCell.Energy,
				static_cast<uint16>(TransferAmount), MutableSourceCell.Energy);
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
				MutableSourceCell.Energy = 0;
				PromoteTopUnderlayer(MutableSourceCell);
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
				SourceCell.Energy = 0;
				PromoteTopUnderlayer(SourceCell);
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
				Swap(SourceCell.Energy, DestinationCell.Energy);
				SourceCell.LastUpdatedTick = Tick;
				DestinationCell.LastUpdatedTick = Tick;
			}
			else
			{
				DestinationCell.MaterialIndex =
					SourceCell.MaterialIndex;
				DestinationCell.Amount = SourceCell.Amount;
				DestinationCell.RemainingLifetime = SourceCell.RemainingLifetime;
				DestinationCell.Energy = SourceCell.Energy;
				DestinationCell.LastUpdatedTick = Tick;
				SourceCell.MaterialIndex = 0;
				SourceCell.Amount = 0;
				SourceCell.RemainingLifetime = 0;
				SourceCell.Energy = 0;
				PromoteTopUnderlayer(SourceCell);
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

			const FMaterialElementAddress FirstAddress =
				FMaterialElementAddress::MakeWorldCell(FIntVector(
					FirstPosition.X, FirstPosition.Y, FirstCell->SupportHeight));
			const FMaterialElementAddress SecondAddress =
				FMaterialElementAddress::MakeWorldCell(FIntVector(
					SecondPosition.X, SecondPosition.Y, SecondCell->SupportHeight));
			const FMaterialElementState FirstState {
				FirstCell->MaterialIndex,
				FirstCell->Amount,
				FirstCell->Energy,
				FirstCell->RemainingLifetime };
			const FMaterialElementState SecondState {
				SecondCell->MaterialIndex,
				SecondCell->Amount,
				SecondCell->Energy,
				SecondCell->RemainingLifetime };
			FLocalMaterialReactionContext Context;
			Context.Seed = Seed;
			Context.LogicalStep = static_cast<int32>(Tick & MAX_int32);
			Context.MaxContacts = 1;
			Context.MaxElementDeltas = 2;
			Context.MaxEmissions = 2;
			FMaterialDeltaBatch Batch;
			FString Error;
			if (!LocalReactionProgram.EvaluatePair(
					FirstAddress,
					FirstState,
					SecondAddress,
					SecondState,
					LocalReactionRevision,
					Context,
					Batch,
					Error)
				|| (Batch.ElementDeltas.IsEmpty()
					&& Batch.ParticleEmissions.IsEmpty()))
			{
				return false;
			}

			for (const FMaterialElementDelta& Delta : Batch.ElementDeltas)
			{
				FCell* Target = Delta.Address == FirstAddress
					? FirstCell
					: Delta.Address == SecondAddress
						? SecondCell
						: nullptr;
				if (!Target)
				{
					return false;
				}
				const FMaterialElementState Current {
					Target->MaterialIndex, Target->Amount, Target->Energy,
					Target->RemainingLifetime };
				if (!(Current == Delta.ExpectedBefore))
				{
					return false;
				}
			}
			for (const FMaterialElementDelta& Delta : Batch.ElementDeltas)
			{
				FCell& Target = Delta.Address == FirstAddress
					? *FirstCell : *SecondCell;
				Target.MaterialIndex = Delta.After.MaterialIndex;
				Target.Amount = Delta.After.Amount;
				Target.Energy = Delta.After.Energy;
				Target.RemainingLifetime = static_cast<uint8>(
					FMath::Min<uint16>(Delta.After.RemainingLifetime, MAX_uint8));
				Target.LastUpdatedTick = Tick;
				PromoteTopUnderlayer(Target);
			}
			QueueReactionEmissions(
				Batch,
				FIntVector(
					FirstPosition.X,
					FirstPosition.Y,
					FirstCell->SupportHeight));
			LocalReactionRevision = Batch.TargetStoreRevision;
			MarkDirtyNeighborhood(FirstPosition);
			MarkDirtyNeighborhood(SecondPosition);
			++Stats.ReactedPairs;
			return true;
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
			&& PowderMaximumStableSlopeAmount > 0
			&& PowderMaximumStableSlopeAmount <= FullCellAmount
			&& BodyWakeRefillDelaySteps >= 0
			&& BodyWakeRefillDelaySteps <= 256
			&& BodyWakeRefillDurationSteps >= 1
			&& BodyWakeRefillDurationSteps <= 256
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
			Material.DefaultEnergy = Definition.DefaultEnergy;
			Material.ConductivityPermille = Definition.ConductivityPermille;
			Material.CoolingPerStep = Definition.CoolingPerStep;
			Material.IgnitionThreshold = Definition.IgnitionThreshold;
			Material.IgnitionEmissionAmount =
				Definition.CombustionEmissionAmount;
			Material.IgnitionSecondaryEmissionAmount =
				Definition.CombustionSecondaryEmissionAmount;
			Impl->MaterialIndices.Add(
				MaterialId,
				static_cast<uint16>(Impl->Materials.Num() - 1));
		}

		for (int32 MaterialIndex = 1;
			MaterialIndex < Impl->Materials.Num();
			++MaterialIndex)
		{
			FImpl::FResolvedMaterial& Material = Impl->Materials[MaterialIndex];
			const FMatterFluxMaterialDefinition& Definition =
				Registry.Materials.FindChecked(Material.Id);
			const auto ResolveThermalMaterial = [&](
				const FName Id, uint16& OutIndex)
			{
				if (Id.IsNone() || Id == TEXT("empty"))
				{
					OutIndex = 0;
					return true;
				}
				if (const uint16* Found = Impl->MaterialIndices.Find(Id))
				{
					OutIndex = *Found;
					return true;
				}
				return false;
			};
			if (!ResolveThermalMaterial(
					Definition.CombustionProduct,
					Material.IgnitionProductMaterialIndex)
				|| !ResolveThermalMaterial(
					Definition.CombustionEmissionMaterial,
					Material.IgnitionEmissionMaterialIndex)
				|| !ResolveThermalMaterial(
					Definition.CombustionSecondaryEmissionMaterial,
					Material.IgnitionSecondaryEmissionMaterialIndex))
			{
				OutError = FString::Printf(
					TEXT("Material '%s' has invalid thermal material references"),
					*Material.Id.ToString());
				Impl = MakeUnique<FImpl>();
				return false;
			}
		}
		if (!Impl->LocalReactionProgram.Compile(Registry, OutError))
		{
			Impl = MakeUnique<FImpl>();
			return false;
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
		Cell.Underlayers.Reset();
		Cell.RemainingLifetime = MaterialIndex == 0
			? 0
			: Impl->Materials[MaterialIndex].LifetimeSteps;
		Cell.Energy = MaterialIndex == 0
			? 0
			: Impl->Materials[MaterialIndex].DefaultEnergy;
		Cell.LastUpdatedTick = 0;
		Cell.BodyDisplacedTick = 0;
		Cell.BodyDisplacedMaterialIndex = 0;
		Cell.BodyDisplacedReferenceAmount = 0;
		Cell.bHasBodyDisplacementVacancy = false;
		Impl->BodyDisplacementVacancies.Remove(WorldCell);
		Impl->MarkDirtyNeighborhood(WorldCell);
		const FIntPoint ChunkCoordinate =
			Impl->ToChunkCoordinate(WorldCell);
		if (!Impl->IsChunkSimulated(ChunkCoordinate))
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
		const uint16* MaterialIndex = Impl->MaterialIndices.Find(MaterialId);
		return MaterialIndex && *MaterialIndex != 0
			? SetCellAmount(
				WorldCell,
				MaterialId,
				Amount,
				Impl->Materials[*MaterialIndex].DefaultEnergy)
			: false;
	}

	bool FChunkedMaterialWorld::SetCellAmount(
		const FIntPoint& WorldCell,
		const FName MaterialId,
		const uint16 Amount,
		const uint16 Energy)
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
		Cell.Underlayers.Reset();
		Cell.RemainingLifetime =
			Impl->Materials[*MaterialIndex].LifetimeSteps;
		Cell.Energy = Energy;
		Cell.LastUpdatedTick = 0;
		Cell.BodyDisplacedTick = 0;
		Cell.BodyDisplacedMaterialIndex = 0;
		Cell.BodyDisplacedReferenceAmount = 0;
		Cell.bHasBodyDisplacementVacancy = false;
		Impl->BodyDisplacementVacancies.Remove(WorldCell);
		Impl->MarkDirtyNeighborhood(WorldCell);
		const FIntPoint ChunkCoordinate =
			Impl->ToChunkCoordinate(WorldCell);
		if (!Impl->IsChunkSimulated(ChunkCoordinate))
		{
			Impl->ArchiveChunk(ChunkCoordinate);
		}
		return true;
	}

	int32 FChunkedMaterialWorld::AddCellAmount(
		const FIntPoint& WorldCell,
		const FName MaterialId,
		const uint16 Amount)
	{
		const uint16* MaterialIndex = Impl->MaterialIndices.Find(MaterialId);
		return MaterialIndex && *MaterialIndex != 0
			? AddCellAmount(
				WorldCell,
				MaterialId,
				Amount,
				Impl->Materials[*MaterialIndex].DefaultEnergy)
			: 0;
	}

	int32 FChunkedMaterialWorld::AddCellAmount(
		const FIntPoint& WorldCell,
		const FName MaterialId,
		const uint16 Amount,
		const uint16 Energy)
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
			return 0;
		}

		const uint16* AddedMaterialIndex =
			Impl->MaterialIndices.Find(MaterialId);
		if (!AddedMaterialIndex || *AddedMaterialIndex == 0)
		{
			return 0;
		}
		FImpl::FCell& Cell = Impl->FindOrAddCell(WorldCell);
		if (Cell.MaterialIndex == 0)
		{
			return SetCellAmount(WorldCell, MaterialId, Amount, Energy)
				? static_cast<int32>(Amount)
				: 0;
		}

		const FImpl::FResolvedMaterial& AddedMaterial =
			Impl->Materials[*AddedMaterialIndex];
		const FImpl::FResolvedMaterial& TopMaterial =
			Impl->Materials[Cell.MaterialIndex];
		if (AddedMaterial.Phase == EMatterFluxMaterialPhase::StaticSolid
			|| TopMaterial.Phase == EMatterFluxMaterialPhase::StaticSolid)
		{
			return 0;
		}

		auto AddToLayer = [Amount](uint16& LayerAmount)
		{
			const uint16 Capacity = MAX_uint16 - LayerAmount;
			const uint16 Accepted = FMath::Min(Amount, Capacity);
			LayerAmount = static_cast<uint16>(LayerAmount + Accepted);
			return Accepted;
		};

		uint16 Accepted = 0;
		if (Cell.MaterialIndex == *AddedMaterialIndex)
		{
			const uint16 PreviousAmount = Cell.Amount;
			Accepted = AddToLayer(Cell.Amount);
			const int64 TotalEnergy =
				static_cast<int64>(PreviousAmount) * Cell.Energy
				+ static_cast<int64>(Accepted) * Energy;
			Cell.Energy = static_cast<uint16>(TotalEnergy / Cell.Amount);
			Cell.RemainingLifetime = AddedMaterial.LifetimeSteps;
			Cell.LastUpdatedTick = 0;
		}
		else if (FImpl::FCell::FLayer* ExistingLayer =
			Cell.Underlayers.FindByPredicate(
				[AddedMaterialIndex](const FImpl::FCell::FLayer& Layer)
				{
					return Layer.MaterialIndex == *AddedMaterialIndex;
				}))
		{
			const uint16 PreviousAmount = ExistingLayer->Amount;
			Accepted = AddToLayer(ExistingLayer->Amount);
			const int64 TotalEnergy =
				static_cast<int64>(PreviousAmount) * ExistingLayer->Energy
				+ static_cast<int64>(Accepted) * Energy;
			ExistingLayer->Energy = static_cast<uint16>(
				TotalEnergy / ExistingLayer->Amount);
			ExistingLayer->RemainingLifetime = AddedMaterial.LifetimeSteps;
			ExistingLayer->LastUpdatedTick = 0;
		}
		else
		{
			FImpl::FCell::FLayer AddedLayer;
			AddedLayer.MaterialIndex = *AddedMaterialIndex;
			AddedLayer.Amount = Amount;
			AddedLayer.RemainingLifetime = AddedMaterial.LifetimeSteps;
			AddedLayer.Energy = Energy;
			Cell.Underlayers.Add(AddedLayer);
			Accepted = Amount;

			// A newly introduced material can change which phase is physically on
			// top. Existing top/underlayer additions keep their identity and must not
			// enter this path: copying an already-updated top into Underlayers duplicates
			// conserved matter and makes every later deposit raise its own support.
			FImpl::FCell::FLayer PreviousTop;
			PreviousTop.MaterialIndex = Cell.MaterialIndex;
			PreviousTop.Amount = Cell.Amount;
			PreviousTop.RemainingLifetime = Cell.RemainingLifetime;
			PreviousTop.Energy = Cell.Energy;
			PreviousTop.LastUpdatedTick = Cell.LastUpdatedTick;
			Cell.Underlayers.Add(PreviousTop);
			Cell.Underlayers.Sort([this](
				const FImpl::FCell::FLayer& A,
				const FImpl::FCell::FLayer& B)
			{
				const uint16 DensityA = Impl->Materials[A.MaterialIndex].Density;
				const uint16 DensityB = Impl->Materials[B.MaterialIndex].Density;
				return DensityA != DensityB
					? DensityA > DensityB
					: A.MaterialIndex < B.MaterialIndex;
			});
			const FImpl::FCell::FLayer NewTop = Cell.Underlayers.Pop(
				EAllowShrinking::No);
			Cell.MaterialIndex = NewTop.MaterialIndex;
			Cell.Amount = NewTop.Amount;
			Cell.RemainingLifetime = NewTop.RemainingLifetime;
			Cell.Energy = NewTop.Energy;
			Cell.LastUpdatedTick = NewTop.LastUpdatedTick;
		}
		Cell.BodyDisplacedTick = 0;
		Cell.BodyDisplacedMaterialIndex = 0;
		Cell.BodyDisplacedReferenceAmount = 0;
		Cell.bHasBodyDisplacementVacancy = false;
		Impl->BodyDisplacementVacancies.Remove(WorldCell);
		Impl->WakeSurfaceFlow(WorldCell);
		Impl->MarkDirtyNeighborhood(WorldCell);
		const FIntPoint ChunkCoordinate = Impl->ToChunkCoordinate(WorldCell);
		if (!Impl->IsChunkSimulated(ChunkCoordinate))
		{
			Impl->ArchiveChunk(ChunkCoordinate);
		}
		return Accepted;
	}

	bool FChunkedMaterialWorld::ReactAirborneParticleAt(
		const FIntPoint& WorldCell,
		const FGuid& ParticleId,
		FName& InOutMaterialId,
		uint16& InOutAmount,
		uint16& InOutEnergy,
		FString& OutError)
	{
		OutError.Reset();
		if (!Impl->bInitialized
			|| !ParticleId.IsValid()
			|| InOutMaterialId.IsNone()
			|| InOutAmount == 0)
		{
			OutError = TEXT("airborne material contact input is invalid");
			return false;
		}
		const uint16* ParticleMaterialIndex =
			Impl->MaterialIndices.Find(InOutMaterialId);
		if (!ParticleMaterialIndex || *ParticleMaterialIndex == 0)
		{
			OutError = TEXT("airborne material is not registered");
			return false;
		}

		FImpl::FCell& Cell = Impl->FindOrAddCell(WorldCell);
		if (Cell.MaterialIndex == 0 || Cell.Amount == 0)
		{
			return false;
		}

		const FMaterialElementAddress WorldAddress =
			FMaterialElementAddress::MakeWorldCell(FIntVector(
				WorldCell.X, WorldCell.Y, Cell.SupportHeight));
		const FMaterialElementAddress ParticleAddress =
			FMaterialElementAddress::MakeAirborneParticle(ParticleId);
		const FMaterialElementState WorldState {
			Cell.MaterialIndex,
			Cell.Amount,
			Cell.Energy,
			Cell.RemainingLifetime };
		const FMaterialElementState ParticleState {
			*ParticleMaterialIndex,
			InOutAmount,
			InOutEnergy,
			0 };
		FLocalMaterialReactionContext Context;
		Context.Seed = Impl->Seed;
		Context.LogicalStep = static_cast<int32>(Impl->Tick & MAX_int32);
		Context.MaxContacts = 1;
		Context.MaxElementDeltas = 2;
		Context.MaxEmissions = 2;
		FMaterialDeltaBatch Batch;
		if (!Impl->LocalReactionProgram.EvaluatePair(
				WorldAddress,
				WorldState,
				ParticleAddress,
				ParticleState,
				Impl->LocalReactionRevision,
				Context,
				Batch,
				OutError))
		{
			return false;
		}
		if (Batch.ElementDeltas.IsEmpty()
			&& Batch.ParticleEmissions.IsEmpty())
		{
			return false;
		}

		for (const FMaterialElementDelta& Delta : Batch.ElementDeltas)
		{
			FMaterialElementState Current;
			if (Delta.Address == WorldAddress)
			{
				Current = { Cell.MaterialIndex, Cell.Amount, Cell.Energy,
					Cell.RemainingLifetime };
			}
			else if (Delta.Address == ParticleAddress)
			{
				Current = { *ParticleMaterialIndex, InOutAmount,
					InOutEnergy, 0 };
			}
			else
			{
				OutError = TEXT("local contact returned an unrelated element delta");
				return false;
			}
			if (!(Current == Delta.ExpectedBefore))
			{
				OutError = TEXT("local contact base state changed before commit");
				return false;
			}
		}

		for (const FMaterialElementDelta& Delta : Batch.ElementDeltas)
		{
			if (Delta.Address == WorldAddress)
			{
				Cell.MaterialIndex = Delta.After.MaterialIndex;
				Cell.Amount = Delta.After.Amount;
				Cell.Energy = Delta.After.Energy;
				Cell.RemainingLifetime = static_cast<uint8>(
					FMath::Min<uint16>(Delta.After.RemainingLifetime, MAX_uint8));
				Cell.LastUpdatedTick = Impl->Tick;
				Impl->PromoteTopUnderlayer(Cell);
			}
			else
			{
				InOutMaterialId = Delta.After.MaterialIndex == 0
					? NAME_None
					: Impl->Materials[Delta.After.MaterialIndex].Id;
				InOutAmount = Delta.After.Amount;
				InOutEnergy = Delta.After.Energy;
			}
		}
		Impl->QueueReactionEmissions(
			Batch,
			FIntVector(WorldCell.X, WorldCell.Y, Cell.SupportHeight));
		Impl->LocalReactionRevision = Batch.TargetStoreRevision;
		Impl->WakeSurfaceFlow(WorldCell);
		Impl->MarkDirtyNeighborhood(WorldCell);
		const FIntPoint ChunkCoordinate = Impl->ToChunkCoordinate(WorldCell);
		if (!Impl->IsChunkSimulated(ChunkCoordinate))
		{
			Impl->ArchiveChunk(ChunkCoordinate);
		}
		return true;
	}

	int32 FChunkedMaterialWorld::AddPowderAmountAtStableSurface(
		const FIntPoint& ImpactCell,
		const FName MaterialId,
		const uint16 Amount,
		const int32 MaximumTravelCells,
		FIntPoint& OutDestinationCell)
	{
		OutDestinationCell = ImpactCell;
		if (!Impl->bInitialized
			|| MaterialId.IsNone()
			|| Amount == 0
			|| MaximumTravelCells < 0)
		{
			return 0;
		}
		const uint16* MaterialIndex = Impl->MaterialIndices.Find(MaterialId);
		if (!MaterialIndex
			|| *MaterialIndex == 0
			|| Impl->Materials[*MaterialIndex].Phase
				!= EMatterFluxMaterialPhase::Powder)
		{
			return 0;
		}

		const auto TryGetPowderSurface255 = [this, MaterialId](
			const FIntPoint& Cell,
			const int32 IncomingAmount,
			int64& OutSurface255)
		{
			const FName ExistingMaterial = GetMaterialAt(Cell);
			if (!ExistingMaterial.IsNone() && ExistingMaterial != MaterialId)
			{
				return false;
			}
			int16 SupportHeight = 0;
			if (!Impl->FindSupportHeight(Cell, SupportHeight))
			{
				return false;
			}
			const int32 ExistingAmount = GetMaterialAmountAt(Cell, MaterialId);
			OutSurface255 = static_cast<int64>(SupportHeight) * FullCellAmount
				+ static_cast<int64>(PowderFullColumnHeight)
					* (ExistingAmount + IncomingAmount);
			return true;
		};

		static const FIntPoint Directions[] = {
			FIntPoint(1, 0), FIntPoint(0, 1), FIntPoint(-1, 0),
			FIntPoint(0, -1), FIntPoint(1, 1), FIntPoint(-1, 1),
			FIntPoint(-1, -1), FIntPoint(1, -1) };
		FIntPoint Current = ImpactCell;
		for (int32 StepIndex = 0;
			StepIndex < MaximumTravelCells;
			++StepIndex)
		{
			int64 CurrentSurfaceAfterDeposit = 0;
			if (!TryGetPowderSurface255(
				Current, Amount, CurrentSurfaceAfterDeposit))
			{
				return 0;
			}

			FIntPoint LowestCell = Current;
			int64 GreatestExcessSurface = 0;
			const uint32 DirectionHash = MixBits(
				Impl->Seed
					^ static_cast<uint32>(ImpactCell.X) * 0x9e3779b9u
					^ static_cast<uint32>(ImpactCell.Y) * 0x85ebca6bu
					^ static_cast<uint32>(StepIndex) * 0xc2b2ae35u
					^ Impl->Tick);
			const int32 FirstDirection = static_cast<int32>(
				DirectionHash % UE_ARRAY_COUNT(Directions));
			for (int32 Offset = 0;
				Offset < UE_ARRAY_COUNT(Directions);
				++Offset)
			{
				FIntPoint Candidate;
				if (!TryOffsetCell(
						Current,
						Directions[(FirstDirection + Offset)
							% UE_ARRAY_COUNT(Directions)],
						Candidate))
				{
					continue;
				}
				int64 CandidateSurface = 0;
				if (!TryGetPowderSurface255(Candidate, 0, CandidateSurface))
				{
					continue;
				}
				const int64 ExcessSurface =
					CurrentSurfaceAfterDeposit - CandidateSurface
						- GetPowderMaximumStableSlope255(
							Impl->Settings, Impl->Seed, Current, Candidate);
				if (ExcessSurface > GreatestExcessSurface)
				{
					GreatestExcessSurface = ExcessSurface;
					LowestCell = Candidate;
				}
			}

			if (LowestCell == Current
				|| GreatestExcessSurface <= 0)
			{
				break;
			}
			Current = LowestCell;
		}

		// Reaching the search budget does not make an unstable powder packet
		// stationary. Commit only if its final surface satisfies the material's
		// repose slope; otherwise the caller keeps the grain airborne.
		int64 FinalSurfaceAfterDeposit = 0;
		if (!TryGetPowderSurface255(Current, Amount, FinalSurfaceAfterDeposit))
		{
			return 0;
		}
		int64 GreatestNeighborExcess = 0;
		for (const FIntPoint Direction : Directions)
		{
			FIntPoint Candidate;
			if (!TryOffsetCell(Current, Direction, Candidate))
			{
				continue;
			}
			int64 CandidateSurface = 0;
			if (TryGetPowderSurface255(Candidate, 0, CandidateSurface))
			{
				GreatestNeighborExcess = FMath::Max(
					GreatestNeighborExcess,
					FinalSurfaceAfterDeposit - CandidateSurface
						- GetPowderMaximumStableSlope255(
							Impl->Settings, Impl->Seed, Current, Candidate));
			}
		}
		if (GreatestNeighborExcess > 0)
		{
			OutDestinationCell = Current;
			return 0;
		}

		const int32 Accepted = AddCellAmount(Current, MaterialId, Amount);
		if (Accepted > 0)
		{
			OutDestinationCell = Current;
		}
		return Accepted;
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
		if (!Impl->IsChunkSimulated(ChunkCoordinate))
		{
			Impl->ArchiveChunk(ChunkCoordinate);
		}
		return true;
	}

	bool FChunkedMaterialWorld::SetExternalSupportHeight(
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

		const int16 ClampedHeight = static_cast<int16>(FMath::Clamp(
			Height,
			static_cast<int32>(MIN_int16),
			static_cast<int32>(MAX_int16)));
		if (const int16* Existing =
			Impl->ExternalSupportHeights.Find(WorldCell);
			Existing && *Existing == ClampedHeight)
		{
			return true;
		}
		Impl->ExternalSupportHeights.Add(WorldCell, ClampedHeight);
		Impl->WakeSurfaceFlow(WorldCell);
		Impl->FindOrAddCell(WorldCell);
		Impl->MarkDirtyNeighborhood(WorldCell);
		return true;
	}

	bool FChunkedMaterialWorld::ClearExternalSupportHeight(
		const FIntPoint& WorldCell)
	{
		if (!Impl->bInitialized
			|| Impl->ExternalSupportHeights.Remove(WorldCell) == 0)
		{
			return false;
		}
		Impl->WakeSurfaceFlow(WorldCell);
		Impl->FindOrAddCell(WorldCell);
		Impl->MarkDirtyNeighborhood(WorldCell);
		return true;
	}

	bool FChunkedMaterialWorld::SeedSurface(
		const TArray<FSeedCell>& SeedCells)
	{
		return SeedSurface(SeedCells, true);
	}

	bool FChunkedMaterialWorld::SeedSurface(
		const TArray<FSeedCell>& SeedCells,
		const bool bFinalizeBaseline)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MatterFlux_MaterialWorld_SeedSurface);
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
			const bool bReplacedMaterial = Cell.MaterialIndex != 0
				|| !Cell.Underlayers.IsEmpty();
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
			Cell.Underlayers.Reset();
			Cell.RemainingLifetime = Cell.MaterialIndex == 0
				? 0
				: Impl->Materials[Cell.MaterialIndex].LifetimeSteps;
			Cell.Energy = Cell.MaterialIndex == 0
				? 0
				: SeedCell.Energy != 0
					? SeedCell.Energy
					: Impl->Materials[Cell.MaterialIndex].DefaultEnergy;
			Cell.LastUpdatedTick = 0;
			Cell.BodyDisplacedTick = 0;
			Cell.BodyDisplacedMaterialIndex = 0;
			Cell.BodyDisplacedReferenceAmount = 0;
			Cell.bHasBodyDisplacementVacancy = false;
			Impl->BodyDisplacementVacancies.Remove(SeedCell.WorldCell);
			// Terrain topology is a support fact, not active falling-sand work.
			// The generated map seeds every terrain coordinate, most of which has
			// no material at all. Waking those empty cells expanded each active
			// chunk's sparse dirty rectangle to the full 64x64 area and made the
			// first playable material steps scan the landscape. A material cell
			// wakes its own halo; an empty-to-empty support seed has nothing for
			// the solver or liquid projection to update.
			if (bReplacedMaterial || Cell.MaterialIndex != 0)
			{
				Impl->MarkDirtyNeighborhood(SeedCell.WorldCell);
			}
			TouchedChunks.Add(
				Impl->ToChunkCoordinate(SeedCell.WorldCell));
		}

		if (!bFinalizeBaseline)
		{
			return true;
		}

		TRACE_CPUPROFILER_EVENT_SCOPE(
			MatterFlux_MaterialWorld_FinalizeSurfaceBaseline);
		for (const FIntPoint& ChunkCoordinate : TouchedChunks)
		{
			if (!Impl->IsChunkSimulated(ChunkCoordinate))
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
			if (Impl->SurfaceFlowChunks.Contains(ChunkCoordinate))
			{
				Impl->MarkChunkFlowMaterialsDirty(ChunkCoordinate);
			}
			else if (Impl->IsChunkActive(ChunkCoordinate))
			{
				Impl->MarkChunkForBaselineResume(ChunkCoordinate);
			}
		}
		return true;
	}

	void FChunkedMaterialWorld::WakeSurfaceCells(
		const TConstArrayView<FIntPoint> WorldCells)
	{
		if (!Impl->bInitialized
			|| !Impl->Settings.bUseSurfaceTopology
			|| WorldCells.IsEmpty())
		{
			return;
		}

		TSet<FIntPoint> UniqueChunks;
		for (const FIntPoint WorldCell : WorldCells)
		{
			if (Impl->Settings.bCullOutsideSurfaceBounds
				&& !Impl->IsInsideSurfaceBounds(WorldCell))
			{
				continue;
			}
			UniqueChunks.Add(Impl->ToChunkCoordinate(WorldCell));
		}
		TArray<FIntPoint> OrderedChunks = UniqueChunks.Array();
		OrderedChunks.Sort([](const FIntPoint A, const FIntPoint B)
		{
			return A.X != B.X ? A.X < B.X : A.Y < B.Y;
		});
		for (const FIntPoint ChunkCoordinate : OrderedChunks)
		{
			if (!Impl->IsChunkActive(ChunkCoordinate))
			{
				Impl->SurfaceFlowChunks.Add(ChunkCoordinate);
			}
			Impl->FindOrAddChunk(ChunkCoordinate);
			// Generated baseline liquids are intentionally dormant when restored
			// through ordinary player focus. An explicit visibility wake means the
			// caller wants one real settling solve, including pristine water. Keep
			// that wake sparse: terrain support and empty cells are facts, not work.
			Impl->MarkChunkFlowMaterialsDirty(ChunkCoordinate);
		}
		Impl->TrimTransientSurfaceFlowChunks();
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

		TMap<FIntPoint, uint16> MaximumRemainingAmounts;
		MaximumRemainingAmounts.Reserve(Constraints.Num());
		for (const FLiquidDisplacementConstraint& Constraint : Constraints)
		{
			uint16* Existing = MaximumRemainingAmounts.Find(
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
		// The vacancy timestamp means "last physically occupied", not "first
		// displaced". Reasserting an idempotent constraint over an already empty
		// cell must refresh it even though no additional amount moves.
		for (const FIntPoint OccupiedCell : StableOccupiedCells)
		{
			FImpl::FCell* Cell = Impl->FindCell(OccupiedCell);
			if (Cell && Cell->bHasBodyDisplacementVacancy)
			{
				Cell->BodyDisplacedTick = Impl->Tick;
			}
		}

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
			uint16 Energy = 0;
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
					SourceCell->RemainingLifetime,
					SourceCell->Energy });
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
				int64 TransferTotalEnergy = 0;
				int32 SourceMinX = MAX_int32;
				int32 SourceMaxX = MIN_int32;
				int32 SourceMinY = MAX_int32;
				int32 SourceMaxY = MIN_int32;
				for (const FSourceState& Source : Sources)
				{
					RemainingToMove += Source.AmountToMove;
					TransferTotalEnergy +=
						static_cast<int64>(Source.AmountToMove) * Source.Energy;
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
				const uint16 TransferEnergy = RemainingToMove > 0
					? static_cast<uint16>(TransferTotalEnergy / RemainingToMove)
					: 0;
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
								// A recent body wake owns its own bounded, conserved
								// restitution path. Feeding it directly from a new body's
								// pressure transaction lets a walking footprint refill its
								// trailing edge in the same render frame.
								if (CandidateCell
									&& CandidateCell->bHasBodyDisplacementVacancy)
								{
									continue;
								}
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
					DestinationCell.Energy = MergeSpecificEnergy(
						DestinationCell.Amount, DestinationCell.Energy,
						static_cast<uint16>(Destination.TransferAmount),
						TransferEnergy);
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
					DestinationCell.Energy = TransferEnergy;
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
						SourceCell->Energy = 0;
						Impl->PromoteTopUnderlayer(*SourceCell);
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

	int32 FChunkedMaterialWorld::DisplacePowders(
		const TConstArrayView<FLiquidDisplacementConstraint> Constraints,
		const int32 MaxSearchRadius)
	{
		if (!Impl->bInitialized || Constraints.IsEmpty() || MaxSearchRadius <= 0)
		{
			return 0;
		}

		TMap<FIntPoint, uint16> MaximumRemainingAmounts;
		MaximumRemainingAmounts.Reserve(Constraints.Num());
		for (const FLiquidDisplacementConstraint& Constraint : Constraints)
		{
			uint16* Existing = MaximumRemainingAmounts.Find(Constraint.WorldCell);
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
		TSet<FIntPoint> OccupiedSet;
		OccupiedSet.Reserve(StableOccupiedCells.Num());
		for (const FIntPoint Cell : StableOccupiedCells)
		{
			OccupiedSet.Add(Cell);
		}

		// Keep independent bodies independent. A nearby lower valley must not
		// teleport sand from another actor's footprint merely because both actors
		// submitted constraints in the same frame.
		TSet<FIntPoint> Unvisited = OccupiedSet;
		TArray<TArray<FIntPoint>> ConnectedFootprints;
		for (const FIntPoint Start : StableOccupiedCells)
		{
			if (Unvisited.Remove(Start) == 0)
			{
				continue;
			}
			TArray<FIntPoint>& Footprint =
				ConnectedFootprints.AddDefaulted_GetRef();
			TArray<FIntPoint> Queue = { Start };
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
							&& Unvisited.Remove(Neighbor) > 0)
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

		struct FSource
		{
			FIntPoint WorldCell = FIntPoint::ZeroValue;
			int32 AmountToMove = 0;
			uint8 RemainingLifetime = 0;
			uint16 Energy = 0;
		};
		struct FDestination
		{
			FIntPoint WorldCell = FIntPoint::ZeroValue;
			int64 SurfaceHeight255 = 0;
			uint32 StableOrder = 0;
			uint16 Amount = 0;
			int32 TransferAmount = 0;
		};

		int32 MovedCells = 0;
		TSet<FIntPoint> TouchedChunks;
		const int32 SearchRadius = FMath::Clamp(MaxSearchRadius, 1, 256);
		for (const TArray<FIntPoint>& Footprint : ConnectedFootprints)
		{
			TMap<uint16, TArray<FSource>> SourcesByMaterial;
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
							== EMatterFluxMaterialPhase::Powder)
					{
						SourceCell = &Impl->FindOrAddCell(Source);
						TouchedChunks.Add(Impl->ToChunkCoordinate(Source));
					}
				}
				if (!SourceCell
					|| SourceCell->MaterialIndex == 0
					|| SourceCell->MaterialIndex >= Impl->Materials.Num()
					|| SourceCell->Amount == 0
					|| Impl->Materials[SourceCell->MaterialIndex].Phase
						!= EMatterFluxMaterialPhase::Powder)
				{
					continue;
				}
				const int32 AmountToMove = FMath::Max(
					static_cast<int32>(SourceCell->Amount)
						- static_cast<int32>(
							MaximumRemainingAmounts.FindChecked(Source)),
					0);
				if (AmountToMove > 0)
				{
					SourcesByMaterial.FindOrAdd(SourceCell->MaterialIndex).Add({
						Source, AmountToMove, SourceCell->RemainingLifetime,
						SourceCell->Energy });
				}
			}

			TArray<uint16> MaterialIndices;
			SourcesByMaterial.GetKeys(MaterialIndices);
			MaterialIndices.Sort();
			for (const uint16 MaterialIndex : MaterialIndices)
			{
				TArray<FSource>& Sources = SourcesByMaterial.FindChecked(MaterialIndex);
				int64 RemainingToMove = 0;
				uint8 TransferLifetime = 0;
				int64 TransferTotalEnergy = 0;
				for (const FSource& Source : Sources)
				{
					RemainingToMove += Source.AmountToMove;
					TransferTotalEnergy +=
						static_cast<int64>(Source.AmountToMove) * Source.Energy;
					if (Source.RemainingLifetime != 0)
					{
						TransferLifetime = TransferLifetime == 0
							? Source.RemainingLifetime
							: FMath::Min(TransferLifetime, Source.RemainingLifetime);
					}
				}
				const int64 InitialAmountToMove = RemainingToMove;
				const uint16 TransferEnergy = RemainingToMove > 0
					? static_cast<uint16>(TransferTotalEnergy / RemainingToMove)
					: 0;
				TArray<FDestination> Destinations;
				TSet<FIntPoint> AddedDestinationCells;
				int64 DestinationCapacity = 0;
				const uint32 FootprintHash = GetTypeHash(Footprint[0]);
				for (int32 Radius = 1;
					Radius <= SearchRadius && DestinationCapacity < RemainingToMove;
					++Radius)
				{
					TSet<FIntPoint> RingCells;
					for (const FIntPoint Source : Footprint)
					{
						for (int32 Offset = -Radius; Offset <= Radius; ++Offset)
						{
							const FIntPoint Offsets[4] = {
								FIntPoint(Offset, -Radius), FIntPoint(Radius, Offset),
								FIntPoint(Offset, Radius), FIntPoint(-Radius, Offset)
							};
							for (const FIntPoint CellOffset : Offsets)
							{
								FIntPoint Candidate;
								if (TryOffsetCell(Source, CellOffset, Candidate)
									&& !OccupiedSet.Contains(Candidate))
								{
									RingCells.Add(Candidate);
								}
							}
						}
					}
					TArray<FIntPoint> StableRing = RingCells.Array();
					StableRing.Sort([](const FIntPoint A, const FIntPoint B)
					{
						return A.Y != B.Y ? A.Y < B.Y : A.X < B.X;
					});
					for (const FIntPoint Candidate : StableRing)
					{
						if (AddedDestinationCells.Contains(Candidate)
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
						const FImpl::FCell* CandidateCell = Impl->FindCell(Candidate);
						const uint16 CandidateMaterial = CandidateCell
							? CandidateCell->MaterialIndex
							: Impl->FindArchivedMaterialIndex(Candidate);
						if (CandidateMaterial != 0 && CandidateMaterial != MaterialIndex)
						{
							continue;
						}
						const uint16 CandidateAmount = CandidateCell
							? CandidateCell->Amount
							: Impl->FindArchivedAmount(Candidate);
						if (CandidateAmount == MAX_uint16)
						{
							continue;
						}
						Destinations.Add({
							Candidate,
							static_cast<int64>(SupportHeight) * FullCellAmount
								+ static_cast<int64>(PowderFullColumnHeight)
									* CandidateAmount,
							HashCombine(FootprintHash, GetTypeHash(Candidate)),
							CandidateAmount,
							0 });
						AddedDestinationCells.Add(Candidate);
						DestinationCapacity += MAX_uint16 - CandidateAmount;
					}
				}
				if (Destinations.IsEmpty())
				{
					continue;
				}

				const int64 TransferTarget = FMath::Min(
					RemainingToMove, DestinationCapacity);
				int64 Low = MAX_int64;
				int64 High = MIN_int64;
				for (const FDestination& Destination : Destinations)
				{
					Low = FMath::Min(Low, Destination.SurfaceHeight255);
					High = FMath::Max(
						High,
						Destination.SurfaceHeight255
							+ static_cast<int64>(MAX_uint16 - Destination.Amount - 1)
								* PowderFullColumnHeight);
				}
				while (Low < High)
				{
					const int64 Middle = Low + (High - Low) / 2;
					int64 PlacementCount = 0;
					for (const FDestination& Destination : Destinations)
					{
						if (Middle >= Destination.SurfaceHeight255)
						{
							PlacementCount += FMath::Min<int64>(
								MAX_uint16 - Destination.Amount,
								(Middle - Destination.SurfaceHeight255)
									/ PowderFullColumnHeight + 1);
						}
						if (PlacementCount >= TransferTarget)
						{
							break;
						}
					}
					if (PlacementCount >= TransferTarget)
					{
						High = Middle;
					}
					else
					{
						Low = Middle + 1;
					}
				}
				const int64 Cutoff = Low;
				int64 Placed = 0;
				TArray<int32> Ties;
				for (int32 Index = 0; Index < Destinations.Num(); ++Index)
				{
					FDestination& Destination = Destinations[Index];
					const int32 Capacity = MAX_uint16 - Destination.Amount;
					if (Cutoff > Destination.SurfaceHeight255)
					{
						Destination.TransferAmount = static_cast<int32>(
							FMath::Min<int64>(
								Capacity,
								(Cutoff - 1 - Destination.SurfaceHeight255)
									/ PowderFullColumnHeight + 1));
					}
					Placed += Destination.TransferAmount;
					if (Destination.TransferAmount < Capacity
						&& Destination.SurfaceHeight255
							+ static_cast<int64>(Destination.TransferAmount)
								* PowderFullColumnHeight == Cutoff)
					{
						Ties.Add(Index);
					}
				}
				Ties.Sort([&Destinations](const int32 A, const int32 B)
				{
					return Destinations[A].StableOrder < Destinations[B].StableOrder;
				});
				for (const int32 Index : Ties)
				{
					if (Placed >= TransferTarget)
					{
						break;
					}
					++Destinations[Index].TransferAmount;
					++Placed;
				}

				for (const FDestination& Destination : Destinations)
				{
					if (Destination.TransferAmount <= 0)
					{
						continue;
					}
					FImpl::FCell& DestinationCell =
						Impl->FindOrAddCell(Destination.WorldCell);
					const bool bWasEmpty = DestinationCell.MaterialIndex == 0;
					DestinationCell.Energy = MergeSpecificEnergy(
						DestinationCell.Amount, DestinationCell.Energy,
						static_cast<uint16>(Destination.TransferAmount),
						TransferEnergy);
					DestinationCell.MaterialIndex = MaterialIndex;
					DestinationCell.Amount = static_cast<uint16>(
						static_cast<int32>(DestinationCell.Amount)
							+ Destination.TransferAmount);
					if (TransferLifetime != 0)
					{
						DestinationCell.RemainingLifetime = bWasEmpty
							|| DestinationCell.RemainingLifetime == 0
							? TransferLifetime
							: FMath::Min(
								DestinationCell.RemainingLifetime,
								TransferLifetime);
					}
					DestinationCell.LastUpdatedTick = Impl->Tick;
					TouchedChunks.Add(
						Impl->ToChunkCoordinate(Destination.WorldCell));
					Impl->MarkDirtyNeighborhood(Destination.WorldCell);
				}

				int64 SourceAmountToConsume = Placed;
				for (const FSource& Source : Sources)
				{
					if (SourceAmountToConsume <= 0)
					{
						break;
					}
					FImpl::FCell* SourceCell = Impl->FindCell(Source.WorldCell);
					if (!SourceCell || SourceCell->MaterialIndex != MaterialIndex)
					{
						continue;
					}
					const int32 Transfer = static_cast<int32>(FMath::Min3<int64>(
						SourceAmountToConsume,
						Source.AmountToMove,
						SourceCell->Amount));
					SourceCell->Amount = static_cast<uint16>(
						static_cast<int32>(SourceCell->Amount) - Transfer);
					if (SourceCell->Amount == 0)
					{
						SourceCell->MaterialIndex = 0;
						SourceCell->RemainingLifetime = 0;
						SourceCell->Energy = 0;
						Impl->PromoteTopUnderlayer(*SourceCell);
					}
					SourceCell->LastUpdatedTick = Impl->Tick;
					TouchedChunks.Add(Impl->ToChunkCoordinate(Source.WorldCell));
					Impl->MarkDirtyNeighborhood(Source.WorldCell);
					SourceAmountToConsume -= Transfer;
					++MovedCells;
				}
				check(SourceAmountToConsume == 0);
				RemainingToMove -= Placed;
				check(RemainingToMove == InitialAmountToMove - Placed);
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

	uint16 FChunkedMaterialWorld::GetMaterialAmountAt(
		const FIntPoint& WorldCell,
		const FName MaterialId) const
	{
		if (!Impl->bInitialized)
		{
			return 0;
		}
		const uint16* MaterialIndex = Impl->MaterialIndices.Find(MaterialId);
		if (!MaterialIndex)
		{
			return 0;
		}
		if (const FImpl::FCell* Cell = Impl->FindCell(WorldCell))
		{
			if (Cell->MaterialIndex == *MaterialIndex)
			{
				return Cell->Amount;
			}
			if (const FImpl::FCell::FLayer* Layer =
				Cell->Underlayers.FindByPredicate(
					[MaterialIndex](const FImpl::FCell::FLayer& Candidate)
					{
						return Candidate.MaterialIndex == *MaterialIndex;
					}))
			{
				return Layer->Amount;
			}
			return 0;
		}
		const FImpl::FArchivedChunk* Archive =
			Impl->ArchivedChunks.Find(Impl->ToChunkCoordinate(WorldCell));
		if (!Archive)
		{
			return 0;
		}
		const uint16 CellIndex = static_cast<uint16>(
			Impl->ToIndex(Impl->ToLocalCoordinate(WorldCell)));
		if (const TArray<FImpl::FCell::FLayer>* Layers =
			Archive->UnderlayersByCell.Find(CellIndex))
		{
			if (const FImpl::FCell::FLayer* Layer = Layers->FindByPredicate(
				[MaterialIndex](const FImpl::FCell::FLayer& Candidate)
				{
					return Candidate.MaterialIndex == *MaterialIndex;
				}))
			{
				return Layer->Amount;
			}
		}
		return Impl->FindArchivedMaterialIndex(WorldCell) == *MaterialIndex
			? Impl->FindArchivedAmount(WorldCell)
			: 0;
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
		OutSnapshot.Energy = Cell
			? Cell->Energy
			: Impl->FindArchivedEnergy(WorldCell);
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
				Count += Cell.Underlayers.ContainsByPredicate(
					[MaterialIndex](const FImpl::FCell::FLayer& Layer)
					{
						return Layer.MaterialIndex == *MaterialIndex;
					}) ? 1 : 0;
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
			for (const TPair<uint16, TArray<FImpl::FCell::FLayer>>& Layers
				: Pair.Value.UnderlayersByCell)
			{
				Count += Layers.Value.ContainsByPredicate(
					[MaterialIndex](const FImpl::FCell::FLayer& Layer)
					{
						return Layer.MaterialIndex == *MaterialIndex;
					}) ? 1 : 0;
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
				for (const FImpl::FCell::FLayer& Layer : Cell.Underlayers)
				{
					if (Layer.MaterialIndex == *MaterialIndex)
					{
						Total += Layer.Amount;
					}
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
			for (const TPair<uint16, TArray<FImpl::FCell::FLayer>>& Layers
				: Pair.Value.UnderlayersByCell)
			{
				for (const FImpl::FCell::FLayer& Layer : Layers.Value)
				{
					if (Layer.MaterialIndex == *MaterialIndex)
					{
						Total += Layer.Amount;
					}
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
				const FIntPoint WorldCell(
					Pair.Key.X * Impl->Settings.ChunkSize
						+ CellIndex % Impl->Settings.ChunkSize,
					Pair.Key.Y * Impl->Settings.ChunkSize
						+ CellIndex / Impl->Settings.ChunkSize);
				int16 EffectiveSupport =
					Pair.Value->Cells[CellIndex].SupportHeight;
				Impl->FindBaseSupportHeight(WorldCell, EffectiveSupport);
				const FImpl::FCell& Cell = Pair.Value->Cells[CellIndex];
				for (const FImpl::FCell::FLayer& Layer : Cell.Underlayers)
				{
					if (Layer.MaterialIndex == 0
						|| Layer.MaterialIndex >= Impl->Materials.Num())
					{
						continue;
					}
					FCellSnapshot& LayerSnapshot =
						OutCells.AddDefaulted_GetRef();
					LayerSnapshot.WorldCell = WorldCell;
					LayerSnapshot.MaterialId =
						Impl->Materials[Layer.MaterialIndex].Id;
					LayerSnapshot.SupportHeight = EffectiveSupport;
					LayerSnapshot.Amount = Layer.Amount;
					LayerSnapshot.RemainingLifetime = Layer.RemainingLifetime;
					LayerSnapshot.Energy = Layer.Energy;
					EffectiveSupport = static_cast<int16>(FMath::Clamp(
						static_cast<int32>(EffectiveSupport)
							+ Impl->GetLayerHeight(
								Layer.MaterialIndex, Layer.Amount),
						static_cast<int32>(MIN_int16),
						static_cast<int32>(MAX_int16)));
				}
				FCellSnapshot& Snapshot = OutCells.AddDefaulted_GetRef();
				Snapshot.WorldCell = WorldCell;
				Snapshot.MaterialId = Impl->Materials[MaterialIndex].Id;
				Snapshot.SupportHeight = EffectiveSupport;
				Snapshot.Amount = Cell.Amount;
				Snapshot.RemainingLifetime = Cell.RemainingLifetime;
				Snapshot.Energy = Cell.Energy;
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

				const FIntPoint WorldCell(
					Pair.Key.X * Impl->Settings.ChunkSize
						+ CellIndex % Impl->Settings.ChunkSize,
					Pair.Key.Y * Impl->Settings.ChunkSize
						+ CellIndex / Impl->Settings.ChunkSize);
				int16 EffectiveSupport = Cell.SupportHeight;
				Impl->FindBaseSupportHeight(WorldCell, EffectiveSupport);
				for (const FImpl::FCell::FLayer& Layer : Cell.Underlayers)
				{
					if (Layer.MaterialIndex == 0
						|| Layer.MaterialIndex >= Impl->Materials.Num())
					{
						continue;
					}
					FCellSnapshot& LayerSnapshot =
						OutCells.AddDefaulted_GetRef();
					LayerSnapshot.WorldCell = WorldCell;
					LayerSnapshot.MaterialId =
						Impl->Materials[Layer.MaterialIndex].Id;
					LayerSnapshot.SupportHeight = EffectiveSupport;
					LayerSnapshot.Amount = Layer.Amount;
					LayerSnapshot.RemainingLifetime = Layer.RemainingLifetime;
					LayerSnapshot.Energy = Layer.Energy;
					EffectiveSupport = static_cast<int16>(FMath::Clamp(
						static_cast<int32>(EffectiveSupport)
							+ Impl->GetLayerHeight(
								Layer.MaterialIndex, Layer.Amount),
						static_cast<int32>(MIN_int16),
						static_cast<int32>(MAX_int16)));
				}
				FCellSnapshot& Snapshot = OutCells.AddDefaulted_GetRef();
				Snapshot.WorldCell = WorldCell;
				Snapshot.MaterialId = Impl->Materials[Cell.MaterialIndex].Id;
				Snapshot.SupportHeight = EffectiveSupport;
				Snapshot.Amount = Cell.Amount;
				Snapshot.RemainingLifetime = Cell.RemainingLifetime;
				Snapshot.Energy = Cell.Energy;
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

					const FIntPoint WorldCell(
						Pair.Key.X * Impl->Settings.ChunkSize
							+ CellIndex % Impl->Settings.ChunkSize,
						Pair.Key.Y * Impl->Settings.ChunkSize
							+ CellIndex / Impl->Settings.ChunkSize);
					int16 EffectiveSupport = Run.SupportHeight;
					Impl->FindBaseSupportHeight(WorldCell, EffectiveSupport);
					if (const TArray<FImpl::FCell::FLayer>* Layers =
						Pair.Value.UnderlayersByCell.Find(
							static_cast<uint16>(CellIndex)))
					{
						for (const FImpl::FCell::FLayer& Layer : *Layers)
						{
							if (Layer.MaterialIndex == 0
								|| Layer.MaterialIndex >= Impl->Materials.Num())
							{
								continue;
							}
							FCellSnapshot& LayerSnapshot =
								OutCells.AddDefaulted_GetRef();
							LayerSnapshot.WorldCell = WorldCell;
							LayerSnapshot.MaterialId =
								Impl->Materials[Layer.MaterialIndex].Id;
							LayerSnapshot.SupportHeight = EffectiveSupport;
							LayerSnapshot.Amount = Layer.Amount;
							LayerSnapshot.RemainingLifetime = 0;
							LayerSnapshot.Energy = Layer.Energy;
							LayerSnapshot.Energy = Layer.Energy;
							EffectiveSupport = static_cast<int16>(FMath::Clamp(
								static_cast<int32>(EffectiveSupport)
									+ Impl->GetLayerHeight(
										Layer.MaterialIndex, Layer.Amount),
								static_cast<int32>(MIN_int16),
								static_cast<int32>(MAX_int16)));
						}
					}
					FCellSnapshot& Snapshot = OutCells.AddDefaulted_GetRef();
					Snapshot.WorldCell = WorldCell;
					Snapshot.MaterialId = Impl->Materials[Run.MaterialIndex].Id;
					Snapshot.SupportHeight = EffectiveSupport;
					Snapshot.Amount = Run.Amount;
					Snapshot.RemainingLifetime = 0;
					Snapshot.Energy = Run.Energy;
					Snapshot.Energy = Run.Energy;
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

	void FChunkedMaterialWorld::GetResidentCells(
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

				const FIntPoint WorldCell(
					Pair.Key.X * Impl->Settings.ChunkSize
						+ CellIndex % Impl->Settings.ChunkSize,
					Pair.Key.Y * Impl->Settings.ChunkSize
						+ CellIndex / Impl->Settings.ChunkSize);
				int16 EffectiveSupport = Cell.SupportHeight;
				Impl->FindBaseSupportHeight(WorldCell, EffectiveSupport);
				for (const FImpl::FCell::FLayer& Layer : Cell.Underlayers)
				{
					if (Layer.MaterialIndex == 0
						|| Layer.MaterialIndex >= Impl->Materials.Num())
					{
						continue;
					}
					FCellSnapshot& LayerSnapshot =
						OutCells.AddDefaulted_GetRef();
					LayerSnapshot.WorldCell = WorldCell;
					LayerSnapshot.MaterialId =
						Impl->Materials[Layer.MaterialIndex].Id;
					LayerSnapshot.SupportHeight = EffectiveSupport;
					LayerSnapshot.Amount = Layer.Amount;
					LayerSnapshot.RemainingLifetime = Layer.RemainingLifetime;
					LayerSnapshot.Energy = Layer.Energy;
					EffectiveSupport = static_cast<int16>(FMath::Clamp(
						static_cast<int32>(EffectiveSupport)
							+ Impl->GetLayerHeight(
								Layer.MaterialIndex, Layer.Amount),
						static_cast<int32>(MIN_int16),
						static_cast<int32>(MAX_int16)));
				}
				FCellSnapshot& Snapshot = OutCells.AddDefaulted_GetRef();
				Snapshot.WorldCell = WorldCell;
				Snapshot.MaterialId = Impl->Materials[Cell.MaterialIndex].Id;
				Snapshot.SupportHeight = EffectiveSupport;
				Snapshot.Amount = Cell.Amount;
				Snapshot.RemainingLifetime = Cell.RemainingLifetime;
				Snapshot.Energy = Cell.Energy;
			}
		}

		OutCells.Sort([](
			const FCellSnapshot& A,
			const FCellSnapshot& B)
		{
			return A.WorldCell.Y != B.WorldCell.Y
				? A.WorldCell.Y < B.WorldCell.Y
				: A.WorldCell.X < B.WorldCell.X;
		});
	}

	void FChunkedMaterialWorld::GetCellsInChunks(
		const TConstArrayView<FIntPoint> Chunks,
		TArray<FCellSnapshot>& OutCells) const
	{
		OutCells.Reset();
		if (!Impl->bInitialized || Chunks.IsEmpty())
		{
			return;
		}

		TSet<FIntPoint> UniqueChunks;
		UniqueChunks.Reserve(Chunks.Num());
		UniqueChunks.Append(Chunks);
		for (const FIntPoint ChunkCoordinate : UniqueChunks)
		{
			if (const TUniquePtr<FImpl::FChunk>* ResidentChunk =
				Impl->Chunks.Find(ChunkCoordinate))
			{
				for (int32 CellIndex = 0;
					CellIndex < (*ResidentChunk)->Cells.Num();
					++CellIndex)
				{
					const FImpl::FCell& Cell = (*ResidentChunk)->Cells[CellIndex];
					if (Cell.MaterialIndex == 0
						|| Cell.MaterialIndex >= Impl->Materials.Num())
					{
						continue;
					}
					const FIntPoint WorldCell(
						ChunkCoordinate.X * Impl->Settings.ChunkSize
							+ CellIndex % Impl->Settings.ChunkSize,
						ChunkCoordinate.Y * Impl->Settings.ChunkSize
							+ CellIndex / Impl->Settings.ChunkSize);
					int16 EffectiveSupport = Cell.SupportHeight;
					Impl->FindBaseSupportHeight(WorldCell, EffectiveSupport);
					for (const FImpl::FCell::FLayer& Layer : Cell.Underlayers)
					{
						if (Layer.MaterialIndex == 0
							|| Layer.MaterialIndex >= Impl->Materials.Num())
						{
							continue;
						}
						FCellSnapshot& LayerSnapshot =
							OutCells.AddDefaulted_GetRef();
						LayerSnapshot.WorldCell = WorldCell;
						LayerSnapshot.MaterialId =
							Impl->Materials[Layer.MaterialIndex].Id;
						LayerSnapshot.SupportHeight = EffectiveSupport;
						LayerSnapshot.Amount = Layer.Amount;
						LayerSnapshot.RemainingLifetime =
							Layer.RemainingLifetime;
						LayerSnapshot.Energy = Layer.Energy;
						EffectiveSupport = static_cast<int16>(FMath::Clamp(
							static_cast<int32>(EffectiveSupport)
								+ Impl->GetLayerHeight(
									Layer.MaterialIndex, Layer.Amount),
							static_cast<int32>(MIN_int16),
							static_cast<int32>(MAX_int16)));
					}
					FCellSnapshot& Snapshot = OutCells.AddDefaulted_GetRef();
					Snapshot.WorldCell = WorldCell;
					Snapshot.MaterialId =
						Impl->Materials[Cell.MaterialIndex].Id;
					Snapshot.SupportHeight = EffectiveSupport;
					Snapshot.Amount = Cell.Amount;
					Snapshot.RemainingLifetime = Cell.RemainingLifetime;
					Snapshot.Energy = Cell.Energy;
				}
				continue;
			}

			const FImpl::FArchivedChunk* ArchivedChunk =
				Impl->ArchivedChunks.Find(ChunkCoordinate);
			if (!ArchivedChunk)
			{
				continue;
			}
			int32 CellIndex = 0;
			for (const FImpl::FArchivedRun& Run : ArchivedChunk->Runs)
			{
				for (int32 RunIndex = 0; RunIndex < Run.Length;
					++RunIndex, ++CellIndex)
				{
					if (Run.MaterialIndex == 0
						|| Run.MaterialIndex >= Impl->Materials.Num())
					{
						continue;
					}
					const FIntPoint WorldCell(
						ChunkCoordinate.X * Impl->Settings.ChunkSize
							+ CellIndex % Impl->Settings.ChunkSize,
						ChunkCoordinate.Y * Impl->Settings.ChunkSize
							+ CellIndex / Impl->Settings.ChunkSize);
					int16 EffectiveSupport = Run.SupportHeight;
					Impl->FindBaseSupportHeight(WorldCell, EffectiveSupport);
					if (const TArray<FImpl::FCell::FLayer>* Layers =
						ArchivedChunk->UnderlayersByCell.Find(
							static_cast<uint16>(CellIndex)))
					{
						for (const FImpl::FCell::FLayer& Layer : *Layers)
						{
							if (Layer.MaterialIndex == 0
								|| Layer.MaterialIndex >= Impl->Materials.Num())
							{
								continue;
							}
							FCellSnapshot& LayerSnapshot =
								OutCells.AddDefaulted_GetRef();
							LayerSnapshot.WorldCell = WorldCell;
							LayerSnapshot.MaterialId =
								Impl->Materials[Layer.MaterialIndex].Id;
							LayerSnapshot.SupportHeight = EffectiveSupport;
							LayerSnapshot.Amount = Layer.Amount;
							LayerSnapshot.RemainingLifetime = 0;
							EffectiveSupport = static_cast<int16>(FMath::Clamp(
								static_cast<int32>(EffectiveSupport)
									+ Impl->GetLayerHeight(
										Layer.MaterialIndex, Layer.Amount),
								static_cast<int32>(MIN_int16),
								static_cast<int32>(MAX_int16)));
						}
					}
					FCellSnapshot& Snapshot = OutCells.AddDefaulted_GetRef();
					Snapshot.WorldCell = WorldCell;
					Snapshot.MaterialId =
						Impl->Materials[Run.MaterialIndex].Id;
					Snapshot.SupportHeight = EffectiveSupport;
					Snapshot.Amount = Run.Amount;
					Snapshot.RemainingLifetime = 0;
				}
			}
		}

		OutCells.Sort([](
			const FCellSnapshot& A,
			const FCellSnapshot& B)
		{
			return A.WorldCell.Y != B.WorldCell.Y
				? A.WorldCell.Y < B.WorldCell.Y
				: A.WorldCell.X < B.WorldCell.X;
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

	void FChunkedMaterialWorld::ConsumeReactionEmissions(
		TArray<FReactionEmission>& OutEmissions)
	{
		OutEmissions = MoveTemp(Impl->PendingReactionEmissions);
		Impl->PendingReactionEmissions.Reset();
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
			uint16 Energy = 0;
			TArray<FImpl::FCell::FLayer> Underlayers;
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
					&& Cell.RemainingLifetime == 0
					&& (Cell.MaterialIndex == 0
						|| Cell.Energy
							== Impl->Materials[Cell.MaterialIndex].DefaultEnergy)
					&& Cell.Underlayers.IsEmpty())
				{
					continue;
				}
				Chunk.Cells.Add({
					static_cast<uint32>(CellIndex),
					Cell.MaterialIndex,
					Cell.SupportHeight,
					BaselineSupportHeight,
					CurrentAmount,
					Cell.RemainingLifetime,
					Cell.Energy,
					Cell.Underlayers });
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
				const bool bHasEnergy = Cell.MaterialIndex != 0
					&& Cell.Energy
						!= Impl->Materials[Cell.MaterialIndex].DefaultEnergy;
				AppendVarUint32(
					OutState,
					(static_cast<uint32>(Cell.MaterialIndex) << 4u)
						| (bHasEnergy ? 8u : 0u)
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
				if (bHasEnergy)
				{
					AppendVarUint32(OutState, Cell.Energy);
				}
				AppendVarUint32(
					OutState,
					static_cast<uint32>(Cell.Underlayers.Num()));
				for (const FImpl::FCell::FLayer& Layer : Cell.Underlayers)
				{
					AppendVarUint32(OutState, Layer.MaterialIndex);
					AppendVarUint32(OutState, Layer.Amount);
					AppendUint8(OutState, Layer.RemainingLifetime);
					AppendVarUint32(OutState, Layer.Energy);
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
			uint16 Energy = 0;
			TArray<FImpl::FCell::FLayer> Underlayers;
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
					&& (PackedMaterialCode >> 4u) == 0)
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
				const bool bHasEncodedEnergy =
					(PackedMaterialCode & 8u) != 0;
				if (bCellEncodingValid && bHasEncodedEnergy)
				{
					uint32 EncodedEnergy = 0;
					bCellEncodingValid = ReadVarUint32(
						State, Offset, EncodedEnergy)
						&& EncodedEnergy <= MAX_uint16;
					if (bCellEncodingValid)
					{
						Cell.Energy = static_cast<uint16>(EncodedEnergy);
					}
				}
				uint32 UnderlayerCount = 0;
				if (bCellEncodingValid)
				{
					bCellEncodingValid = ReadVarUint32(
						State, Offset, UnderlayerCount)
						&& UnderlayerCount <= 16;
				}
				Cell.Underlayers.Reserve(static_cast<int32>(UnderlayerCount));
				for (uint32 LayerIndex = 0;
					bCellEncodingValid && LayerIndex < UnderlayerCount;
					++LayerIndex)
				{
					uint32 EncodedMaterial = 0;
					uint32 EncodedAmount = 0;
					uint32 EncodedEnergy = 0;
					uint8 EncodedLifetime = 0;
					bCellEncodingValid = ReadVarUint32(
							State, Offset, EncodedMaterial)
						&& ReadVarUint32(State, Offset, EncodedAmount)
						&& ReadUint8(State, Offset, EncodedLifetime)
						&& ReadVarUint32(State, Offset, EncodedEnergy)
						&& EncodedMaterial > 0
						&& EncodedMaterial < static_cast<uint32>(Impl->Materials.Num())
						&& EncodedAmount > 0
						&& EncodedAmount <= MAX_uint16;
					bCellEncodingValid = bCellEncodingValid
						&& EncodedEnergy <= MAX_uint16;
					if (bCellEncodingValid)
					{
						FImpl::FCell::FLayer& Layer =
							Cell.Underlayers.AddDefaulted_GetRef();
						Layer.MaterialIndex = static_cast<uint16>(EncodedMaterial);
						Layer.Amount = static_cast<uint16>(EncodedAmount);
						Layer.RemainingLifetime = EncodedLifetime;
						Layer.Energy = static_cast<uint16>(EncodedEnergy);
					}
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
						? PackedMaterialCode >> 4u
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
					if (!bHasEncodedEnergy)
					{
						Cell.Energy =
							Impl->Materials[Cell.MaterialIndex].DefaultEnergy;
					}
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
				Cell.Energy = 0;
				Cell.Underlayers.Reset();
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
			Pair.Value->MarkAllDirty();
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
				Cell.Energy = ImportedCell.Energy;
				Cell.Underlayers = ImportedCell.Underlayers;
				Cell.LastUpdatedTick = 0;
				Cell.BodyDisplacedTick = 0;
				Cell.BodyDisplacedMaterialIndex = 0;
				Cell.BodyDisplacedReferenceAmount = 0;
				Cell.bHasBodyDisplacementVacancy = false;
			}
			Chunk.MarkAllDirty();
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
		Impl->TrimTransientSurfaceFlowChunks();
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
				if (!Impl->IsChunkSimulated(Pair.Key)
					|| !Chunk.HasDirtyCells())
				{
					continue;
				}

				for (const int32 CellIndex : Chunk.DirtyCellIndices)
				{
					CellsToVisit.Add(FIntPoint(
						Pair.Key.X * Impl->Settings.ChunkSize
							+ CellIndex % Impl->Settings.ChunkSize,
						Pair.Key.Y * Impl->Settings.ChunkSize
							+ CellIndex / Impl->Settings.ChunkSize));
				}
				Chunk.ResetDirty();
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
			Stats.CandidateCells = CellsToVisit.Num();
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
				Cell->Energy = 0;
				Impl->PromoteTopUnderlayer(*Cell);
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
				if (Impl->Tick - DestinationCell->BodyDisplacedTick
					<= static_cast<uint32>(
						Impl->Settings.BodyWakeRefillDelaySteps))
				{
					continue;
				}
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
					DestinationCell->Energy = MergeSpecificEnergy(
						DestinationCell->Amount, DestinationCell->Energy,
						static_cast<uint16>(TransferAmount), SourceCell->Energy);
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
						SourceCell->Energy = 0;
						Impl->PromoteTopUnderlayer(*SourceCell);
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
					GetBodyWakeTransferBudget(
						static_cast<int32>(
							DestinationCell->BodyDisplacedReferenceAmount)
							- static_cast<int32>(DestinationCell->Amount),
						DestinationCell->BodyDisplacedReferenceAmount,
						Impl->Settings.BodyWakeRefillDurationSteps) });
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
						FImpl::FCell* DestinationCell =
							Impl->FindCell(Destination.WorldCell);
						if (!DestinationCell)
						{
							continue;
						}

						// Body displacement distributes one column across several
						// nearby surplus columns. Consume as many nearest donors as
						// needed to satisfy this step's pressure budget; limiting a
						// vacancy to one donor made refill speed depend on the
						// accidental per-donor split and felt unresponsive.
						while (Destination.RemainingTransfer > 0)
						{
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
								break;
							}

							FRestitutionDonor& Donor =
								ComponentDonors[BestDonorIndex];
							FImpl::FCell* SourceCell =
								Impl->FindCell(Donor.WorldCell);
							if (!SourceCell)
							{
								Donor.AvailableAmount = 0;
								continue;
							}
							const int32 TransferAmount = FMath::Min(
								Destination.RemainingTransfer,
								Donor.AvailableAmount);
							if (TransferAmount <= 0)
							{
								Donor.AvailableAmount = 0;
								continue;
							}
							DestinationCell->Energy = MergeSpecificEnergy(
								DestinationCell->Amount, DestinationCell->Energy,
								static_cast<uint16>(TransferAmount), SourceCell->Energy);
							DestinationCell->MaterialIndex =
								RestitutionMaterialIndex;
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
								SourceCell->Energy = 0;
								Impl->PromoteTopUnderlayer(*SourceCell);
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
					// 由扩散率决定稳定深度：水形成浅而宽的水洼，熔岩等
					// 低扩散液体保留更深、更紧凑的形状。
					const float TargetAverageAmount = static_cast<float>(
						GetLiquidTargetAverageAmount(
							Impl->Materials[RootCell->MaterialIndex].Dispersion));
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

		bool bMovedSurfacePowder = false;
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
					const int32 MovedBefore = Stats.MovedCells;
					if (Impl->TryMoveSurfaceUnderlayerPowder(
							WorldCell, Stats))
					{
						bMovedSurfacePowder = true;
						continue;
					}
					Impl->TryMoveSurfaceCell(
						WorldCell,
						Material,
						Stats,
						SurfaceLiquidShapes.Find(WorldCell));
					bMovedSurfacePowder |= bIsPowder
						&& Stats.MovedCells > MovedBefore;
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

		// Surface topology stores a vertical powder column in one coarse world
		// cell. Limiting it to one lateral neighbor per 30 Hz fixed step made a
		// spell-sized pile take several seconds merely to cross a tree crown. Run a
		// few powder-only local substeps so granular velocity matches the world-cell
		// scale. Every substep still transfers solely between adjacent cells; this is
		// falling-sand relaxation, not a search for or teleport to the final surface.
		if (Impl->Settings.bUseSurfaceTopology && bMovedSurfacePowder)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(
				MatterFlux_MaterialStep_RelaxSurfacePowder);
			constexpr int32 ExtraPowderRelaxationPasses = 3;
			for (int32 Pass = 0; Pass < ExtraPowderRelaxationPasses; ++Pass)
			{
				TArray<FIntPoint> PowderCells;
				for (const TPair<FIntPoint, TUniquePtr<FImpl::FChunk>>& Pair
					: Impl->Chunks)
				{
					if (!Impl->IsChunkSimulated(Pair.Key)
						|| !Pair.Value->HasDirtyCells())
					{
						continue;
					}
					for (const int32 CellIndex : Pair.Value->DirtyCellIndices)
					{
						FImpl::FCell& Cell = Pair.Value->Cells[CellIndex];
						if (Cell.MaterialIndex == 0
							|| Cell.MaterialIndex >= Impl->Materials.Num()
							|| Impl->Materials[Cell.MaterialIndex].Phase
								!= EMatterFluxMaterialPhase::Powder)
						{
							continue;
						}
						// LastUpdatedTick is the per-substep collision guard. Reset it for
						// candidates, then a moved source and destination are each barred
						// from moving again until the following pass.
						Cell.LastUpdatedTick = 0;
						PowderCells.Add(FIntPoint(
							Pair.Key.X * Impl->Settings.ChunkSize
								+ CellIndex % Impl->Settings.ChunkSize,
							Pair.Key.Y * Impl->Settings.ChunkSize
								+ CellIndex / Impl->Settings.ChunkSize));
					}
				}
				if (PowderCells.IsEmpty())
				{
					break;
				}
				PowderCells.Sort([Order = Impl->Tick + Pass + 1](
					const FIntPoint& A,
					const FIntPoint& B)
				{
					if (A.Y != B.Y)
					{
						return A.Y < B.Y;
					}
					return (Order & 1u) == 0 ? A.X < B.X : A.X > B.X;
				});

				bool bMovedThisPass = false;
				for (const FIntPoint WorldCell : PowderCells)
				{
					FImpl::FCell* Cell = Impl->FindCell(WorldCell);
					if (!Cell || Cell->MaterialIndex == 0
						|| Cell->MaterialIndex >= Impl->Materials.Num()
						|| Cell->LastUpdatedTick == Impl->Tick)
					{
						continue;
					}
					const FImpl::FResolvedMaterial Material =
						Impl->Materials[Cell->MaterialIndex];
					if (Material.Phase != EMatterFluxMaterialPhase::Powder
						|| !Impl->PassesMobilityCheck(WorldCell, Material))
					{
						continue;
					}
					const int32 MovedBefore = Stats.MovedCells;
					++Stats.VisitedCells;
					Impl->TryMoveSurfaceCell(WorldCell, Material, Stats, nullptr);
					bMovedThisPass |= Stats.MovedCells > MovedBefore;
				}
				if (!bMovedThisPass)
				{
					break;
				}
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
					|| !(**Chunk).HasDirtyCells())
				{
					SettledFlowChunks.Add(ChunkCoordinate);
				}
			}
			for (const FIntPoint ChunkCoordinate : SettledFlowChunks)
			{
				Impl->SurfaceFlowChunks.Remove(ChunkCoordinate);
				Impl->ArchiveChunk(ChunkCoordinate);
			}
			Impl->TrimTransientSurfaceFlowChunks();
		}
		return Stats;
	}
}
