#include "Material/MatterFluxGroundStateChunk.h"

#include "Misc/Compression.h"
#include "Misc/Crc.h"

namespace
{
	constexpr int32 GroundChunkSize = 64;
	constexpr int32 ChunkCellCount = GroundChunkSize * GroundChunkSize;
	constexpr int32 ResidueByteCount = ChunkCellCount / 8;
	constexpr int32 PackedByteCount = ResidueByteCount + ChunkCellCount;
	constexpr int32 MaximumPayloadBytes = PackedByteCount;
}

bool FMatterFluxGroundStateChunk::Encode(
	const FIntPoint InChunkCoordinate,
	const int32 InRevision,
	const TArray<uint8>& ResidueMask,
	const TArray<uint8>& BurningMask,
	const int32 WorldWidth,
	const int32 WorldHeight,
	FString& OutError)
{
	OutError.Reset();
	StateBytes.Reset();
	if (WorldWidth <= 0 || WorldHeight <= 0
		|| WorldWidth % GroundChunkSize != 0
		|| WorldHeight % GroundChunkSize != 0
		|| ResidueMask.Num() != WorldWidth * WorldHeight
		|| BurningMask.Num() != ResidueMask.Num()
		|| InChunkCoordinate.X < 0 || InChunkCoordinate.Y < 0
		|| InChunkCoordinate.X >= WorldWidth / GroundChunkSize
		|| InChunkCoordinate.Y >= WorldHeight / GroundChunkSize)
	{
		OutError = TEXT("Ground state chunk dimensions are invalid");
		return false;
	}

	TArray<uint8> Packed;
	Packed.Init(0, PackedByteCount);
	for (int32 LocalY = 0; LocalY < GroundChunkSize; ++LocalY)
	{
		for (int32 LocalX = 0; LocalX < GroundChunkSize; ++LocalX)
		{
			const int32 LocalIndex = LocalY * GroundChunkSize + LocalX;
			const int32 WorldIndex =
				(InChunkCoordinate.Y * GroundChunkSize + LocalY) * WorldWidth
				+ InChunkCoordinate.X * GroundChunkSize + LocalX;
			if (ResidueMask[WorldIndex] > 1)
			{
				OutError = TEXT("Ground residue mask must be binary");
				return false;
			}
			Packed[LocalIndex / 8] |= static_cast<uint8>(
				ResidueMask[WorldIndex] << (LocalIndex % 8));
			Packed[ResidueByteCount + LocalIndex] = BurningMask[WorldIndex];
		}
	}

	ChunkCoordinate = InChunkCoordinate;
	Revision = InRevision;
	StateHash = FCrc::MemCrc32(Packed.GetData(), Packed.Num());
	const int32 Bound = FCompression::CompressMemoryBound(
		NAME_Zlib, Packed.Num(), COMPRESS_BiasSpeed);
	TArray<uint8> Compressed;
	Compressed.SetNumUninitialized(Bound);
	int32 CompressedBytes = Bound;
	bCompressed = FCompression::CompressMemory(
		NAME_Zlib,
		Compressed.GetData(),
		CompressedBytes,
		Packed.GetData(),
		Packed.Num(),
		COMPRESS_BiasSpeed)
		&& CompressedBytes > 0
		&& CompressedBytes < Packed.Num();
	if (bCompressed)
	{
		Compressed.SetNum(CompressedBytes, EAllowShrinking::Yes);
		StateBytes = MoveTemp(Compressed);
	}
	else
	{
		StateBytes = MoveTemp(Packed);
	}
	return true;
}

bool FMatterFluxGroundStateChunk::DecodeInto(
	TArray<uint8>& InOutResidueMask,
	TArray<uint8>& InOutBurningMask,
	const int32 WorldWidth,
	const int32 WorldHeight,
	FString& OutError) const
{
	OutError.Reset();
	if (WorldWidth <= 0 || WorldHeight <= 0
		|| WorldWidth % GroundChunkSize != 0
		|| WorldHeight % GroundChunkSize != 0
		|| InOutResidueMask.Num() != WorldWidth * WorldHeight
		|| InOutBurningMask.Num() != InOutResidueMask.Num()
		|| Revision < 0
		|| ChunkCoordinate.X < 0 || ChunkCoordinate.Y < 0
		|| ChunkCoordinate.X >= WorldWidth / GroundChunkSize
		|| ChunkCoordinate.Y >= WorldHeight / GroundChunkSize
		|| StateBytes.IsEmpty() || StateBytes.Num() > MaximumPayloadBytes)
	{
		OutError = TEXT("Replicated ground chunk metadata is invalid");
		return false;
	}

	TArray<uint8> Packed;
	if (bCompressed)
	{
		Packed.SetNumUninitialized(PackedByteCount);
		if (!FCompression::UncompressMemory(
			NAME_Zlib,
			Packed.GetData(),
			Packed.Num(),
			StateBytes.GetData(),
			StateBytes.Num()))
		{
			OutError = TEXT("Replicated ground chunk decompression failed");
			return false;
		}
	}
	else
	{
		if (StateBytes.Num() != PackedByteCount)
		{
			OutError = TEXT("Raw replicated ground chunk has an invalid length");
			return false;
		}
		Packed = StateBytes;
	}
	if (FCrc::MemCrc32(Packed.GetData(), Packed.Num()) != StateHash)
	{
		OutError = TEXT("Replicated ground chunk CRC validation failed");
		return false;
	}

	for (int32 LocalY = 0; LocalY < GroundChunkSize; ++LocalY)
	{
		for (int32 LocalX = 0; LocalX < GroundChunkSize; ++LocalX)
		{
			const int32 LocalIndex = LocalY * GroundChunkSize + LocalX;
			const int32 WorldIndex =
				(ChunkCoordinate.Y * GroundChunkSize + LocalY) * WorldWidth
				+ ChunkCoordinate.X * GroundChunkSize + LocalX;
			InOutResidueMask[WorldIndex] =
				(Packed[LocalIndex / 8] >> (LocalIndex % 8)) & 1;
			InOutBurningMask[WorldIndex] =
				Packed[ResidueByteCount + LocalIndex];
		}
	}
	return true;
}

bool FMatterFluxGroundStateChunk::NetSerialize(
	FArchive& Ar,
	UPackageMap* Map,
	bool& bOutSuccess)
{
	(void)Map;
	bOutSuccess = false;
	Ar << ChunkCoordinate;
	Ar << Revision;
	Ar << StateHash;
	Ar.SerializeBits(&bCompressed, 1);
	uint16 PayloadBytes = Ar.IsSaving()
		? static_cast<uint16>(FMath::Clamp(
			StateBytes.Num(), 0, MaximumPayloadBytes))
		: 0;
	Ar.SerializeBits(&PayloadBytes, 13);
	if (PayloadBytes > MaximumPayloadBytes
		|| (Ar.IsSaving() && PayloadBytes != StateBytes.Num()))
	{
		Ar.SetError();
		return true;
	}
	if (Ar.IsLoading())
	{
		StateBytes.SetNumUninitialized(PayloadBytes);
	}
	if (PayloadBytes > 0)
	{
		Ar.Serialize(StateBytes.GetData(), PayloadBytes);
	}
	bOutSuccess = !Ar.IsError() && PayloadBytes > 0;
	return true;
}
