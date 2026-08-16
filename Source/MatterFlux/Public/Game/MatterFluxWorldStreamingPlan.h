#pragma once

#include "CoreMinimal.h"

namespace MatterFlux::WorldStreaming
{
	/**
	 * Describes the union of square chunk windows around one or more foci.
	 * Empty WindowOffsets means a single zero offset.
	 */
	struct MATTERFLUX_API FChunkWindowRequest
	{
		TArray<FIntPoint> FocusChunks;
		TArray<FIntPoint> WindowOffsets;
		int32 Radius = 0;
		int32 MaximumChunkCount = 1;
	};

	/**
	 * Builds a unique, X/Y lexicographically sorted chunk list. Validation and
	 * budget failures leave OutChunks unchanged and explain the rejection.
	 */
	MATTERFLUX_API bool BuildChunkWindow(
		const FChunkWindowRequest& Request,
		TArray<FIntPoint>& OutChunks,
		FString& OutError);

	/** Selects the oldest inactive chunk with a stable X/Y tie-break. */
	MATTERFLUX_API bool SelectEvictionCandidate(
		TConstArrayView<FIntPoint> ResidentChunks,
		const TSet<FIntPoint>& ActiveChunks,
		const TMap<FIntPoint, uint64>& LastUsedGeneration,
		FIntPoint& OutChunk);
}
