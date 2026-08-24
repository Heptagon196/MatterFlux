#include "Material/MatterFluxLogicalSourceReactionIndex.h"

namespace MatterFlux::Reaction
{
	bool FLogicalSourceReactionIndex::ApplySnapshot(
		const FGuid& SourceId,
		const bool bHasReactionState,
		const TConstArrayView<uint8> ActiveMask)
	{
		if (!SourceId.IsValid())
		{
			return false;
		}

		const bool bIsActive = bHasReactionState
			&& ActiveMask.ContainsByPredicate([](const uint8 Value)
			{
				return Value != 0;
			});
		if (bIsActive)
		{
			ActiveSourceIds.Add(SourceId);
		}
		else
		{
			ActiveSourceIds.Remove(SourceId);
		}
		return true;
	}

	void FLogicalSourceReactionIndex::Remove(const FGuid& SourceId)
	{
		ActiveSourceIds.Remove(SourceId);
	}

	void FLogicalSourceReactionIndex::Reset()
	{
		ActiveSourceIds.Reset();
	}

	bool FLogicalSourceReactionIndex::Contains(const FGuid& SourceId) const
	{
		return ActiveSourceIds.Contains(SourceId);
	}

	int32 FLogicalSourceReactionIndex::Num() const
	{
		return ActiveSourceIds.Num();
	}

	void FLogicalSourceReactionIndex::GatherStableIds(
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
