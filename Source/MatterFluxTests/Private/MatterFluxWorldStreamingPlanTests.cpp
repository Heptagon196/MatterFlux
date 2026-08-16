#include "Game/MatterFluxWorldStreamingPlan.h"

#include "Algo/Reverse.h"
#include "Misc/AutomationTest.h"

namespace
{
	bool IsLexicographicallySorted(const TArray<FIntPoint>& Chunks)
	{
		for (int32 Index = 1; Index < Chunks.Num(); ++Index)
		{
			const FIntPoint Previous = Chunks[Index - 1];
			const FIntPoint Current = Chunks[Index];
			if (Current.X < Previous.X
				|| (Current.X == Previous.X && Current.Y <= Previous.Y))
			{
				return false;
			}
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxStreamingWindowDeterminismTest,
	"MatterFlux.Streaming.Plan.MultiFocusWindowIsDeterministic",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxStreamingWindowDeterminismTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::WorldStreaming::FChunkWindowRequest Request;
	Request.FocusChunks = {
		FIntPoint(2, -1),
		FIntPoint(-3, 4),
		FIntPoint(2, -1)
	};
	Request.WindowOffsets = {
		FIntPoint::ZeroValue,
		FIntPoint(1, -1),
		FIntPoint::ZeroValue
	};
	Request.Radius = 1;
	Request.MaximumChunkCount = 64;

	TArray<FIntPoint> First;
	FString Error;
	if (!TestTrue(
		TEXT("Multi-focus window builds"),
		MatterFlux::WorldStreaming::BuildChunkWindow(
			Request,
			First,
			Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Overlapping windows are deduplicated"), First.Num(), 28);
	TestTrue(
		TEXT("Output uses strict X/Y lexicographic order"),
		IsLexicographicallySorted(First));
	TestTrue(TEXT("Negative chunk is present"), First.Contains(FIntPoint(-4, 3)));
	TestTrue(TEXT("Camera-offset chunk is present"), First.Contains(FIntPoint(4, -3)));

	Algo::Reverse(Request.FocusChunks);
	Algo::Reverse(Request.WindowOffsets);
	TArray<FIntPoint> Reordered;
	Error.Reset();
	TestTrue(
		TEXT("Reordered request builds"),
		MatterFlux::WorldStreaming::BuildChunkWindow(
			Request,
			Reordered,
			Error));
	TestTrue(
		TEXT("Input order and duplicates do not change output"),
		First == Reordered);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxStreamingWindowCameraUnionTest,
	"MatterFlux.Streaming.Plan.CameraOffsetUsesStableUnion",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxStreamingWindowCameraUnionTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::WorldStreaming::FChunkWindowRequest Request;
	Request.FocusChunks = { FIntPoint::ZeroValue };
	Request.WindowOffsets = {
		FIntPoint::ZeroValue,
		FIntPoint(1, -1)
	};
	Request.Radius = 1;
	Request.MaximumChunkCount = 14;

	TArray<FIntPoint> Chunks;
	FString Error;
	TestTrue(
		TEXT("Isometric camera union builds at its exact budget"),
		MatterFlux::WorldStreaming::BuildChunkWindow(
			Request,
			Chunks,
			Error));
	TestEqual(TEXT("Two overlapping 3x3 windows contain 14 chunks"), Chunks.Num(), 14);
	TestTrue(TEXT("Player corner remains resident"), Chunks.Contains(FIntPoint(-1, 1)));
	TestTrue(TEXT("Camera corner remains resident"), Chunks.Contains(FIntPoint(2, -2)));
	TestTrue(TEXT("Camera union is strictly sorted"), IsLexicographicallySorted(Chunks));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxStreamingWindowTransactionalFailureTest,
	"MatterFlux.Streaming.Plan.InvalidRequestPreservesOutput",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxStreamingWindowTransactionalFailureTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::WorldStreaming::FChunkWindowRequest Request;
	Request.FocusChunks = { FIntPoint::ZeroValue };
	Request.Radius = 1;
	Request.MaximumChunkCount = 8;
	TArray<FIntPoint> Output = { FIntPoint(17, 23) };
	const TArray<FIntPoint> Original = Output;
	FString Error;
	TestFalse(
		TEXT("Insufficient budget is rejected"),
		MatterFlux::WorldStreaming::BuildChunkWindow(
			Request,
			Output,
			Error));
	TestTrue(TEXT("Budget failure preserves previous output"), Output == Original);
	TestFalse(TEXT("Budget failure explains itself"), Error.IsEmpty());

	Request.Radius = -1;
	Request.MaximumChunkCount = 64;
	Error.Reset();
	TestFalse(
		TEXT("Negative radius is rejected"),
		MatterFlux::WorldStreaming::BuildChunkWindow(
			Request,
			Output,
			Error));
	TestTrue(TEXT("Validation failure preserves previous output"), Output == Original);

	Request.Radius = 0;
	Request.FocusChunks = { FIntPoint(MAX_int32, 0) };
	Request.WindowOffsets = { FIntPoint(1, 0) };
	Error.Reset();
	TestFalse(
		TEXT("Coordinate overflow is rejected"),
		MatterFlux::WorldStreaming::BuildChunkWindow(
			Request,
			Output,
			Error));
	TestTrue(TEXT("Overflow failure preserves previous output"), Output == Original);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxStreamingStableEvictionTest,
	"MatterFlux.Streaming.Plan.LruEvictionUsesStableTieBreak",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxStreamingStableEvictionTest::RunTest(
	const FString& Parameters)
{
	TArray<FIntPoint> Residents = {
		FIntPoint(5, 1),
		FIntPoint(0, 0),
		FIntPoint(3, 3),
		FIntPoint(-2, 8)
	};
	TSet<FIntPoint> Active = { FIntPoint(0, 0) };
	TMap<FIntPoint, uint64> LastUsed;
	LastUsed.Add(FIntPoint(5, 1), 7);
	LastUsed.Add(FIntPoint(-2, 8), 7);
	LastUsed.Add(FIntPoint(3, 3), 9);

	FIntPoint Candidate(99, 99);
	TestTrue(
		TEXT("An inactive eviction candidate is found"),
		MatterFlux::WorldStreaming::SelectEvictionCandidate(
			Residents,
			Active,
			LastUsed,
			Candidate));
	TestEqual(
		TEXT("Equal LRU generations use X/Y tie-break"),
		Candidate,
		FIntPoint(-2, 8));

	Algo::Reverse(Residents);
	Candidate = FIntPoint(99, 99);
	TestTrue(
		TEXT("Reordered residents still produce a candidate"),
		MatterFlux::WorldStreaming::SelectEvictionCandidate(
			Residents,
			Active,
			LastUsed,
			Candidate));
	TestEqual(
		TEXT("Resident iteration order does not affect eviction"),
		Candidate,
		FIntPoint(-2, 8));

	for (const FIntPoint Chunk : Residents)
	{
		Active.Add(Chunk);
	}
	Candidate = FIntPoint(17, 23);
	TestFalse(
		TEXT("All-active cache has no eviction candidate"),
		MatterFlux::WorldStreaming::SelectEvictionCandidate(
			Residents,
			Active,
			LastUsed,
			Candidate));
	TestEqual(
		TEXT("No-candidate result preserves output"),
		Candidate,
		FIntPoint(17, 23));
	return true;
}
