#include "Material/MatterFluxLogicalSourceCombustionIndex.h"

namespace MatterFlux::Combustion
{
	bool FLogicalSourceCombustionIndex::ApplySnapshot(
		const FGuid& SourceId,
		const bool bHasCombustionState,
		const TConstArrayView<uint8> BurningMask)
	{
		if (!SourceId.IsValid())
		{
			return false;
		}

		const bool bIsBurning = bHasCombustionState
			&& BurningMask.ContainsByPredicate([](const uint8 Value)
			{
				return Value != 0;
			});
		if (bIsBurning)
		{
			ActiveSourceIds.Add(SourceId);
		}
		else
		{
			ActiveSourceIds.Remove(SourceId);
		}
		return true;
	}

	void FLogicalSourceCombustionIndex::Remove(const FGuid& SourceId)
	{
		ActiveSourceIds.Remove(SourceId);
	}

	void FLogicalSourceCombustionIndex::Reset()
	{
		ActiveSourceIds.Reset();
	}

	bool FLogicalSourceCombustionIndex::Contains(const FGuid& SourceId) const
	{
		return ActiveSourceIds.Contains(SourceId);
	}

	int32 FLogicalSourceCombustionIndex::Num() const
	{
		return ActiveSourceIds.Num();
	}

	void FLogicalSourceCombustionIndex::GatherStableIds(
		TArray<FGuid>& OutSourceIds) const
	{
		OutSourceIds.Reset(ActiveSourceIds.Num());
		for (const FGuid& SourceId : ActiveSourceIds)
		{
			OutSourceIds.Add(SourceId);
		}
		OutSourceIds.Sort([](const FGuid& Left, const FGuid& Right)
		{
			if (Left.A != Right.A)
			{
				return Left.A < Right.A;
			}
			if (Left.B != Right.B)
			{
				return Left.B < Right.B;
			}
			if (Left.C != Right.C)
			{
				return Left.C < Right.C;
			}
			return Left.D < Right.D;
		});
	}
}
