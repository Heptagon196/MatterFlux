#include "Game/MatterFluxWorldStreamingPlan.h"

namespace MatterFlux::WorldStreaming
{
	bool BuildChunkWindow(
		const FChunkWindowRequest& Request,
		TArray<FIntPoint>& OutChunks,
		FString& OutError)
	{
		OutError.Reset();
		constexpr int32 AbsoluteMaximumChunkCount = 1024 * 1024;
		if (Request.Radius < 0
			|| Request.MaximumChunkCount <= 0
			|| Request.MaximumChunkCount > AbsoluteMaximumChunkCount)
		{
			OutError = TEXT("Chunk window radius or output budget is invalid.");
			return false;
		}
		if (Request.FocusChunks.Num() > Request.MaximumChunkCount
			|| Request.WindowOffsets.Num() > Request.MaximumChunkCount)
		{
			OutError = TEXT("Chunk window input exceeds its bounded request size.");
			return false;
		}

		const int64 Diameter =
			static_cast<int64>(Request.Radius) * 2 + 1;
		const int64 ChunksPerWindow = Diameter * Diameter;
		if (!Request.FocusChunks.IsEmpty()
			&& ChunksPerWindow > Request.MaximumChunkCount)
		{
			OutError = TEXT("A single chunk window exceeds the output budget.");
			return false;
		}

		TSet<FIntPoint> UniqueFoci;
		UniqueFoci.Reserve(Request.FocusChunks.Num());
		for (const FIntPoint Focus : Request.FocusChunks)
		{
			UniqueFoci.Add(Focus);
		}
		TSet<FIntPoint> UniqueOffsets;
		if (Request.WindowOffsets.IsEmpty())
		{
			UniqueOffsets.Add(FIntPoint::ZeroValue);
		}
		else
		{
			UniqueOffsets.Reserve(Request.WindowOffsets.Num());
			for (const FIntPoint Offset : Request.WindowOffsets)
			{
				UniqueOffsets.Add(Offset);
			}
		}
		auto SortChunks = [](TArray<FIntPoint>& Chunks)
		{
			Chunks.Sort(
				[](const FIntPoint A, const FIntPoint B)
				{
					return A.X != B.X ? A.X < B.X : A.Y < B.Y;
				});
		};
		TArray<FIntPoint> OrderedFoci = UniqueFoci.Array();
		TArray<FIntPoint> OrderedOffsets = UniqueOffsets.Array();
		SortChunks(OrderedFoci);
		SortChunks(OrderedOffsets);

		TSet<FIntPoint> CandidateSet;
		CandidateSet.Reserve(FMath::Min(
			Request.MaximumChunkCount,
			static_cast<int32>(FMath::Min<int64>(
				static_cast<int64>(UniqueFoci.Num())
					* UniqueOffsets.Num()
					* ChunksPerWindow,
				MAX_int32))));
		for (const FIntPoint Focus : OrderedFoci)
		{
			for (const FIntPoint Offset : OrderedOffsets)
			{
				const int64 CenterX =
					static_cast<int64>(Focus.X) + Offset.X;
				const int64 CenterY =
					static_cast<int64>(Focus.Y) + Offset.Y;
				for (int32 DeltaY = -Request.Radius;
					DeltaY <= Request.Radius;
					++DeltaY)
				{
					for (int32 DeltaX = -Request.Radius;
						DeltaX <= Request.Radius;
						++DeltaX)
					{
						const int64 X = CenterX + DeltaX;
						const int64 Y = CenterY + DeltaY;
						if (X < MIN_int32 || X > MAX_int32
							|| Y < MIN_int32 || Y > MAX_int32)
						{
							OutError = TEXT("Chunk window coordinate overflowed int32.");
							return false;
						}
						const FIntPoint Chunk(
							static_cast<int32>(X),
							static_cast<int32>(Y));
						if (!CandidateSet.Contains(Chunk)
							&& CandidateSet.Num()
								>= Request.MaximumChunkCount)
						{
							OutError = TEXT("Chunk window exceeds the output budget.");
							return false;
						}
						CandidateSet.Add(Chunk);
					}
				}
			}
		}

		TArray<FIntPoint> Candidate = CandidateSet.Array();
		SortChunks(Candidate);
		OutChunks = MoveTemp(Candidate);
		return true;
	}

	bool SelectEvictionCandidate(
		const TConstArrayView<FIntPoint> ResidentChunks,
		const TSet<FIntPoint>& ActiveChunks,
		const TMap<FIntPoint, uint64>& LastUsedGeneration,
		FIntPoint& OutChunk)
	{
		FIntPoint Candidate = FIntPoint::ZeroValue;
		uint64 OldestGeneration = MAX_uint64;
		bool bFound = false;
		for (const FIntPoint Chunk : ResidentChunks)
		{
			if (ActiveChunks.Contains(Chunk))
			{
				continue;
			}
			const uint64 Generation =
				LastUsedGeneration.FindRef(Chunk);
			const bool bLexicographicallyEarlier =
				Chunk.X < Candidate.X
				|| (Chunk.X == Candidate.X && Chunk.Y < Candidate.Y);
			if (!bFound
				|| Generation < OldestGeneration
				|| (Generation == OldestGeneration
					&& bLexicographicallyEarlier))
			{
				Candidate = Chunk;
				OldestGeneration = Generation;
				bFound = true;
			}
		}
		if (!bFound)
		{
			return false;
		}
		OutChunk = Candidate;
		return true;
	}
}
