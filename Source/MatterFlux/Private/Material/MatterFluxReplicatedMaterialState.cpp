#include "Material/MatterFluxReplicatedMaterialState.h"

#include "Misc/Compression.h"
#include "Misc/Crc.h"

namespace
{
	constexpr int32 MaximumMaterialSnapshotBytes = 1024 * 1024;
	// Keep the property payload well below an actor-channel bunch so unrelated
	// replicated fields can share the same update safely.
	constexpr int32 MaximumCompressedMaterialSnapshotBytes = 3 * 1024;
}

bool FMatterFluxReplicatedMaterialState::EncodeActiveState(
	const TArray<uint8>& InActiveState,
	FString& OutError)
{
	OutError.Reset();
	UncompressedByteCount = 0;
	StateHash = 0;
	CompressedState.Reset();
	if (InActiveState.IsEmpty()
		|| InActiveState.Num() > MaximumMaterialSnapshotBytes)
	{
		OutError = TEXT("Material snapshot is empty or exceeds its one-megabyte decode budget");
		return false;
	}

	const int32 Bound = FCompression::CompressMemoryBound(
		NAME_Zlib,
		InActiveState.Num(),
		COMPRESS_BiasSpeed);
	if (Bound <= 0)
	{
		OutError = TEXT("UE could not determine a zlib material snapshot bound");
		return false;
	}
	CompressedState.SetNumUninitialized(Bound);
	int32 CompressedBytes = Bound;
	if (!FCompression::CompressMemory(
		NAME_Zlib,
		CompressedState.GetData(),
		CompressedBytes,
		InActiveState.GetData(),
		InActiveState.Num(),
		COMPRESS_BiasSpeed)
		|| CompressedBytes <= 0
		|| CompressedBytes > MaximumCompressedMaterialSnapshotBytes)
	{
		CompressedState.Reset();
		OutError = FString::Printf(
			TEXT("Compressed material snapshot exceeds the safe %d-byte actor-channel budget"),
			MaximumCompressedMaterialSnapshotBytes);
		return false;
	}
	CompressedState.SetNum(CompressedBytes, EAllowShrinking::Yes);
	UncompressedByteCount = InActiveState.Num();
	StateHash = FCrc::MemCrc32(
		InActiveState.GetData(),
		InActiveState.Num());
	return true;
}

bool FMatterFluxReplicatedMaterialState::DecodeActiveState(
	TArray<uint8>& OutActiveState,
	FString& OutError) const
{
	OutActiveState.Reset();
	OutError.Reset();
	if (UncompressedByteCount <= 0
		|| UncompressedByteCount > MaximumMaterialSnapshotBytes
		|| CompressedState.IsEmpty()
		|| CompressedState.Num() > MaximumCompressedMaterialSnapshotBytes)
	{
		OutError = TEXT("Replicated material snapshot metadata is invalid");
		return false;
	}
	TArray<uint8> Candidate;
	Candidate.SetNumUninitialized(UncompressedByteCount);
	if (!FCompression::UncompressMemory(
		NAME_Zlib,
		Candidate.GetData(),
		Candidate.Num(),
		CompressedState.GetData(),
		CompressedState.Num())
		|| FCrc::MemCrc32(Candidate.GetData(), Candidate.Num()) != StateHash)
	{
		OutError = TEXT("Replicated material snapshot failed decompression or hash validation");
		return false;
	}
	OutActiveState = MoveTemp(Candidate);
	return true;
}

bool FMatterFluxReplicatedMaterialState::NetSerialize(
	FArchive& Ar,
	UPackageMap* Map,
	bool& bOutSuccess)
{
	(void)Map;
	bOutSuccess = false;
	Ar << MapSeed;
	Ar << Revision;
	Ar << UncompressedByteCount;
	Ar << StateHash;

	uint16 PayloadBytes = Ar.IsSaving()
		? static_cast<uint16>(FMath::Clamp(
			CompressedState.Num(),
			0,
			MaximumCompressedMaterialSnapshotBytes))
		: 0;
	// Thirteen bits cover the application budget. Serializing size and bytes as
	// one property update prevents adjacent revisions from being combined.
	Ar.SerializeBits(&PayloadBytes, 13);
	if (PayloadBytes > MaximumCompressedMaterialSnapshotBytes
		|| (Ar.IsSaving() && PayloadBytes != CompressedState.Num()))
	{
		if (Ar.IsLoading())
		{
			CompressedState.Reset();
		}
		return true;
	}
	if (Ar.IsLoading())
	{
		CompressedState.SetNumUninitialized(PayloadBytes);
	}
	if (PayloadBytes > 0)
	{
		Ar.Serialize(CompressedState.GetData(), PayloadBytes);
	}
	bOutSuccess = !Ar.IsError()
		&& UncompressedByteCount > 0
		&& UncompressedByteCount <= MaximumMaterialSnapshotBytes
		&& PayloadBytes > 0;
	return true;
}
