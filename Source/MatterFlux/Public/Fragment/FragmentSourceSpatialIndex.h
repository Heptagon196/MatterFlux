#pragma once

#include "CoreMinimal.h"

namespace MatterFlux::Fragment
{
	class MATTERFLUX_API FSourceSpatialIndex final
	{
	public:
		explicit FSourceSpatialIndex(double InCellSize = 1024.0);
		~FSourceSpatialIndex();

		FSourceSpatialIndex(const FSourceSpatialIndex&) = delete;
		FSourceSpatialIndex& operator=(const FSourceSpatialIndex&) = delete;

		bool Upsert(const FGuid& SourceId, const FBox& WorldBounds);
		bool Remove(const FGuid& SourceId);
		void Reset();
		void Query(const FBox& WorldBounds, TArray<FGuid>& OutSourceIds) const;
		/** Returns the stable, deduplicated union intersecting any valid bound. */
		void QueryMany(
			TConstArrayView<FBox> WorldBounds,
			TArray<FGuid>& OutSourceIds) const;
		int32 Num() const;

	private:
		struct FImpl;
		TUniquePtr<FImpl> Impl;
	};
}
