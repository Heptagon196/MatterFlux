#pragma once

#include "CoreMinimal.h"
#include "MatterFluxReplicatedMaterialState.generated.h"

class UPackageMap;

/**
 * Atomic, bounded wire representation of the active material simulation.
 *
 * Compression, integrity validation and net serialization intentionally live
 * behind this one interface so the playable-world actor only coordinates the
 * state transition; it does not know the wire format.
 */
USTRUCT()
struct MATTERFLUX_API FMatterFluxReplicatedMaterialState
{
	GENERATED_BODY()

	bool EncodeActiveState(
		const TArray<uint8>& InActiveState,
		FString& OutError);
	bool DecodeActiveState(
		TArray<uint8>& OutActiveState,
		FString& OutError) const;
	bool NetSerialize(
		FArchive& Ar,
		UPackageMap* Map,
		bool& bOutSuccess);

	bool HasPayload() const
	{
		return UncompressedByteCount > 0 && !CompressedState.IsEmpty();
	}

	int32 GetCompressedByteCount() const
	{
		return CompressedState.Num();
	}

	UPROPERTY()
	int32 MapSeed = 0;

	UPROPERTY()
	int32 Revision = 0;

	UPROPERTY()
	int32 UncompressedByteCount = 0;

	UPROPERTY()
	uint32 StateHash = 0;

	UPROPERTY()
	TArray<uint8> CompressedState;
};

template<>
struct TStructOpsTypeTraits<FMatterFluxReplicatedMaterialState>
	: public TStructOpsTypeTraitsBase2<FMatterFluxReplicatedMaterialState>
{
	enum
	{
		WithNetSerializer = true
	};
};
