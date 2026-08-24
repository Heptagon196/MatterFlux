#pragma once

#include "CoreMinimal.h"

namespace MatterFlux::Reaction
{
	/**
	 * Tracks only logical fragment sources that currently contain active
	 * cells. Historical output snapshots stay in the world state store but
	 * do not make visualization work grow over the lifetime of the world.
	 */
	class MATTERFLUX_API FLogicalSourceReactionIndex
	{
	public:
		/** Applies the latest snapshot. Invalid source ids are rejected. */
		bool ApplySnapshot(
			const FGuid& SourceId,
			bool bHasReactionState,
			TConstArrayView<uint8> ActiveMask);

		void Remove(const FGuid& SourceId);
		void Reset();

		bool Contains(const FGuid& SourceId) const;
		int32 Num() const;

		/** Returns platform-stable GUID order, independent of insertion order. */
		void GatherStableIds(TArray<FGuid>& OutSourceIds) const;

	private:
		TSet<FGuid> ActiveSourceIds;
	};
}
