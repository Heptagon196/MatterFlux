#pragma once

#include "CoreMinimal.h"
#include "MatterFluxGroundStateChunk.generated.h"

/** Atomic wire payload for one 64 x 64 ground-reaction chunk. */
USTRUCT()
struct MATTERFLUX_API FMatterFluxGroundStateChunk
{
	GENERATED_BODY()

	bool Encode(
		FIntPoint InChunkCoordinate,
		int32 InRevision,
		const TArray<uint8>& OutputMask,
		const TArray<uint8>& ActiveMask,
		int32 WorldWidth,
		int32 WorldHeight,
		FString& OutError);
	bool DecodeInto(
		TArray<uint8>& InOutOutputMask,
		TArray<uint8>& InOutActiveMask,
		int32 WorldWidth,
		int32 WorldHeight,
		FString& OutError) const;
	bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess);

	UPROPERTY()
	FIntPoint ChunkCoordinate = FIntPoint::ZeroValue;

	UPROPERTY()
	int32 Revision = 0;

	UPROPERTY()
	uint32 StateHash = 0;

	UPROPERTY()
	bool bCompressed = false;

	UPROPERTY()
	TArray<uint8> StateBytes;
};

template<>
struct TStructOpsTypeTraits<FMatterFluxGroundStateChunk>
	: public TStructOpsTypeTraitsBase2<FMatterFluxGroundStateChunk>
{
	enum { WithNetSerializer = true };
};
