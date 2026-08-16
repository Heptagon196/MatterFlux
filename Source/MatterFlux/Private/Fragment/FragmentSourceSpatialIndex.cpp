#include "Fragment/FragmentSourceSpatialIndex.h"

namespace MatterFlux::Fragment
{
	struct FSourceSpatialIndex::FImpl
	{
		struct FEntry
		{
			FBox Bounds = FBox(ForceInit);
			TArray<FIntPoint> Cells;
			bool bGlobal = false;
		};

		explicit FImpl(const double InCellSize)
			: CellSize(
				FMath::IsFinite(InCellSize) && InCellSize > 0.0
					? InCellSize
					: 1024.0)
		{
		}

		static constexpr int64 MaximumCellsPerOperation = 4096;
		double CellSize = 1024.0;
		TMap<FGuid, FEntry> Entries;
		TMap<FIntPoint, TSet<FGuid>> Buckets;
		TSet<FGuid> GlobalEntries;

		bool GetCellRange(
			const FBox& Bounds,
			FIntPoint& OutMin,
			FIntPoint& OutMax,
			int64& OutCellCount) const
		{
			OutCellCount = 0;
			if (!Bounds.IsValid
				|| Bounds.Min.ContainsNaN()
				|| Bounds.Max.ContainsNaN()
				|| !FMath::IsFinite(Bounds.Min.X)
				|| !FMath::IsFinite(Bounds.Min.Y)
				|| !FMath::IsFinite(Bounds.Max.X)
				|| !FMath::IsFinite(Bounds.Max.Y)
				|| Bounds.Min.X > Bounds.Max.X
				|| Bounds.Min.Y > Bounds.Max.Y)
			{
				return false;
			}

			const double MinimumCell =
				static_cast<double>(MIN_int32) * CellSize;
			const double MaximumCell =
				static_cast<double>(MAX_int32) * CellSize;
			if (Bounds.Min.X < MinimumCell
				|| Bounds.Min.Y < MinimumCell
				|| Bounds.Max.X > MaximumCell
				|| Bounds.Max.Y > MaximumCell)
			{
				return false;
			}

			OutMin = FIntPoint(
				FMath::FloorToInt(Bounds.Min.X / CellSize),
				FMath::FloorToInt(Bounds.Min.Y / CellSize));
			OutMax = FIntPoint(
				FMath::FloorToInt(Bounds.Max.X / CellSize),
				FMath::FloorToInt(Bounds.Max.Y / CellSize));
			const int64 Width =
				static_cast<int64>(OutMax.X) - OutMin.X + 1;
			const int64 Height =
				static_cast<int64>(OutMax.Y) - OutMin.Y + 1;
			if (Width <= 0 || Height <= 0
				|| Width > MAX_int32 / Height)
			{
				return false;
			}
			OutCellCount = Width * Height;
			return true;
		}

		void RemoveEntryFromBuckets(
			const FGuid& SourceId,
			const FEntry& Entry)
		{
			if (Entry.bGlobal)
			{
				GlobalEntries.Remove(SourceId);
				return;
			}
			for (const FIntPoint Cell : Entry.Cells)
			{
				TSet<FGuid>* Bucket = Buckets.Find(Cell);
				if (!Bucket)
				{
					continue;
				}
				Bucket->Remove(SourceId);
				if (Bucket->IsEmpty())
				{
					Buckets.Remove(Cell);
				}
			}
		}
	};

	FSourceSpatialIndex::FSourceSpatialIndex(const double InCellSize)
		: Impl(MakeUnique<FImpl>(InCellSize))
	{
	}

	FSourceSpatialIndex::~FSourceSpatialIndex() = default;

	bool FSourceSpatialIndex::Upsert(
		const FGuid& SourceId,
		const FBox& WorldBounds)
	{
		FIntPoint MinCell;
		FIntPoint MaxCell;
		int64 CellCount = 0;
		if (!SourceId.IsValid()
			|| !Impl->GetCellRange(
				WorldBounds,
				MinCell,
				MaxCell,
				CellCount))
		{
			return false;
		}

		FImpl::FEntry Candidate;
		Candidate.Bounds = WorldBounds;
		Candidate.bGlobal =
			CellCount > FImpl::MaximumCellsPerOperation;
		if (!Candidate.bGlobal)
		{
			Candidate.Cells.Reserve(static_cast<int32>(CellCount));
			for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
			{
				for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
				{
					Candidate.Cells.Emplace(X, Y);
				}
			}
		}

		if (const FImpl::FEntry* Existing =
			Impl->Entries.Find(SourceId))
		{
			Impl->RemoveEntryFromBuckets(SourceId, *Existing);
		}
		Impl->Entries.Add(SourceId, Candidate);
		if (Candidate.bGlobal)
		{
			Impl->GlobalEntries.Add(SourceId);
		}
		else
		{
			for (const FIntPoint Cell : Candidate.Cells)
			{
				Impl->Buckets.FindOrAdd(Cell).Add(SourceId);
			}
		}
		return true;
	}

	bool FSourceSpatialIndex::Remove(const FGuid& SourceId)
	{
		FImpl::FEntry Existing;
		if (!Impl->Entries.RemoveAndCopyValue(SourceId, Existing))
		{
			return false;
		}
		Impl->RemoveEntryFromBuckets(SourceId, Existing);
		return true;
	}

	void FSourceSpatialIndex::Reset()
	{
		Impl->Entries.Reset();
		Impl->Buckets.Reset();
		Impl->GlobalEntries.Reset();
	}

	void FSourceSpatialIndex::Query(
		const FBox& WorldBounds,
		TArray<FGuid>& OutSourceIds) const
	{
		QueryMany(MakeArrayView(&WorldBounds, 1), OutSourceIds);
	}

	void FSourceSpatialIndex::QueryMany(
		const TConstArrayView<FBox> WorldBounds,
		TArray<FGuid>& OutSourceIds) const
	{
		OutSourceIds.Reset();
		TArray<int32> ValidBounds;
		ValidBounds.Reserve(WorldBounds.Num());
		TSet<FGuid> Candidates;
		bool bAllEntriesAreCandidates = false;
		for (int32 BoundsIndex = 0;
			BoundsIndex < WorldBounds.Num();
			++BoundsIndex)
		{
			FIntPoint MinCell;
			FIntPoint MaxCell;
			int64 CellCount = 0;
			if (!Impl->GetCellRange(
				WorldBounds[BoundsIndex],
				MinCell,
				MaxCell,
				CellCount))
			{
				continue;
			}
			ValidBounds.Add(BoundsIndex);
			if (bAllEntriesAreCandidates)
			{
				continue;
			}
			if (CellCount > FImpl::MaximumCellsPerOperation)
			{
				bAllEntriesAreCandidates = true;
				Candidates.Reset();
				continue;
			}
			for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
			{
				for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
				{
					if (const TSet<FGuid>* Bucket =
						Impl->Buckets.Find(FIntPoint(X, Y)))
					{
						Candidates.Append(*Bucket);
					}
				}
			}
		}
		if (ValidBounds.IsEmpty())
		{
			return;
		}
		if (bAllEntriesAreCandidates)
		{
			Impl->Entries.GetKeys(OutSourceIds);
		}
		else
		{
			Candidates.Append(Impl->GlobalEntries);
			OutSourceIds.Reserve(Candidates.Num());
			for (const FGuid& SourceId : Candidates)
			{
				OutSourceIds.Add(SourceId);
			}
		}

		for (int32 CandidateIndex = OutSourceIds.Num() - 1;
			CandidateIndex >= 0;
			--CandidateIndex)
		{
			const FGuid SourceId = OutSourceIds[CandidateIndex];
			const FImpl::FEntry* Entry = Impl->Entries.Find(SourceId);
			bool bIntersectsAny = false;
			if (Entry)
			{
				for (const int32 BoundsIndex : ValidBounds)
				{
					if (Entry->Bounds.Intersect(WorldBounds[BoundsIndex]))
					{
						bIntersectsAny = true;
						break;
					}
				}
			}
			if (!bIntersectsAny)
			{
				OutSourceIds.RemoveAtSwap(
					CandidateIndex,
					1,
					EAllowShrinking::No);
			}
		}
		OutSourceIds.Sort(
			[](const FGuid& A, const FGuid& B)
			{
				return A.A != B.A ? A.A < B.A
					: A.B != B.B ? A.B < B.B
					: A.C != B.C ? A.C < B.C
					: A.D < B.D;
			});
	}

	int32 FSourceSpatialIndex::Num() const
	{
		return Impl->Entries.Num();
	}
}
