#include "Game/MatterFluxFragmentSourceReplication.h"

#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr int32 ReplicationBenchmarkCellCount = 1024;
	constexpr int32 ReplicationBenchmarkIterations = 256;

	FFragment2DSourceStreamingState MakeVolumeState(
		const int32 SourceIndex)
	{
		FFragment2DSourceStreamingState State;
		State.Revision = 1;
		TArray<uint8> InputMask;
		InputMask.SetNumUninitialized(ReplicationBenchmarkCellCount);
		for (int32 CellIndex = 0;
			CellIndex < ReplicationBenchmarkCellCount;
			++CellIndex)
		{
			InputMask[CellIndex] =
				((CellIndex + SourceIndex) % 5) != 0 ? 1 : 0;
			if (((CellIndex + SourceIndex) % 64) == 0
				&& InputMask[CellIndex] != 0)
			{
				State.VolumeCellStates.Add({
					FIntVector(CellIndex % 32, CellIndex / 32, 0),
					TEXT("charcoal"),
					static_cast<uint16>(40000 + SourceIndex) });
			}
		}
		State.SetRuntimeMask(MoveTemp(InputMask));
		return State;
	}

	bool BenchmarkSourceBatch(
		FAutomationTestBase& Test,
		const int32 SourceCount,
		double& OutAverageMilliseconds)
	{
		TArray<FFragment2DSourceStreamingState> SourceStates;
		SourceStates.Reserve(SourceCount);
		TArray<FMatterFluxFragmentSourceStateBatchUpdate> Updates;
		Updates.Reserve(SourceCount);
		for (int32 SourceIndex = 0; SourceIndex < SourceCount; ++SourceIndex)
		{
			SourceStates.Add(MakeVolumeState(SourceIndex));
		}
		for (int32 SourceIndex = SourceCount - 1;
			SourceIndex >= 0;
			--SourceIndex)
		{
			Updates.Add(
				{
					FGuid(
						static_cast<uint32>(SourceIndex + 1),
						0x9e3779b9u,
						0x85ebca6bu,
						0xc2b2ae35u),
					&SourceStates[SourceIndex]
				});
		}

		FMatterFluxReplicatedFragmentSourceStateList ReplicatedStates;
		if (!Test.TestEqual(
			TEXT("Warm-up Source batch commits"),
			ReplicatedStates.UpsertAuthorityBatch(
				Updates,
				SourceCount,
				SourceCount * 1024),
			EMatterFluxFragmentSourceStateUpsertResult::Committed))
		{
			return false;
		}
		TArray<const uint8*> RuntimeBuffers;
		RuntimeBuffers.Reserve(SourceCount);
		for (const FMatterFluxReplicatedFragmentSourceState& Item
			: ReplicatedStates.Items)
		{
			RuntimeBuffers.Add(Item.PackedRuntimeMask.GetData());
		}
		const int32 ExpectedPayloadBytes =
			ReplicatedStates.GetAuthorityPayloadByteCount();

		const double StartSeconds = FPlatformTime::Seconds();
		for (int32 Iteration = 0;
			Iteration < ReplicationBenchmarkIterations;
			++Iteration)
		{
			for (FFragment2DSourceStreamingState& State : SourceStates)
			{
				++State.Revision;
				++State.VolumeFieldRevision;
			}
			if (ReplicatedStates.UpsertAuthorityBatch(
				Updates,
				SourceCount,
				SourceCount * 1024)
				!= EMatterFluxFragmentSourceStateUpsertResult::Committed)
			{
				Test.AddError(TEXT("Repeated Source batch was rejected"));
				return false;
			}
		}
		OutAverageMilliseconds =
			(FPlatformTime::Seconds() - StartSeconds)
			* 1000.0
			/ ReplicationBenchmarkIterations;

		if (!Test.TestEqual(
			TEXT("Batch keeps one replicated entry per Source"),
			ReplicatedStates.Items.Num(),
			SourceCount)
			|| !Test.TestEqual(
				TEXT("Batch byte accounting covers one mask and sparse Volume cells"),
				ReplicatedStates.GetAuthorityPayloadByteCount(),
				ExpectedPayloadBytes))
		{
			return false;
		}
		for (int32 ItemIndex = 0;
			ItemIndex < ReplicatedStates.Items.Num();
			++ItemIndex)
		{
			if (!Test.TestTrue(
				TEXT("Equal-size runtime mask updates reuse their payload buffer"),
				RuntimeBuffers[ItemIndex]
					== ReplicatedStates.Items[ItemIndex]
						.PackedRuntimeMask.GetData()))
			{
				return false;
			}
		}
		return true;
	}

	bool BenchmarkClientDeltaPlan(
		FAutomationTestBase& Test,
		const int32 DeltaSourceCount,
		double& OutAverageMilliseconds)
	{
		constexpr int32 HistoricalSourceCount = 4096;
		FMatterFluxReplicatedFragmentSourceStateList States;
		States.Items.Reserve(HistoricalSourceCount);
		for (int32 SourceIndex = 0;
			SourceIndex < HistoricalSourceCount;
			++SourceIndex)
		{
			FMatterFluxReplicatedFragmentSourceState& Item =
				States.Items.AddDefaulted_GetRef();
			Item.SourceId = FGuid(
				static_cast<uint32>(SourceIndex + 1),
				0x9e3779b9u,
				0x85ebca6bu,
				0xc2b2ae35u);
			Item.ReplicationID = SourceIndex + 100;
			States.ItemMap.Add(Item.ReplicationID, SourceIndex);
		}
		FMatterFluxFragmentSourceClientApplyPlan InitialPlan;
		States.ConsumeClientApplyPlan(InitialPlan);
		TArray<int32> ChangedIndices;
		ChangedIndices.Reserve(DeltaSourceCount);
		for (int32 DeltaIndex = DeltaSourceCount - 1;
			DeltaIndex >= 0;
			--DeltaIndex)
		{
			ChangedIndices.Add(HistoricalSourceCount - 1 - DeltaIndex);
		}

		const double StartSeconds = FPlatformTime::Seconds();
		for (int32 Iteration = 0;
			Iteration < ReplicationBenchmarkIterations;
			++Iteration)
		{
			States.PostReplicatedChange(
				MakeArrayView(ChangedIndices),
				HistoricalSourceCount);
			FMatterFluxFragmentSourceClientApplyPlan DeltaPlan;
			States.ConsumeClientApplyPlan(DeltaPlan);
			if (DeltaPlan.bFullRebuild
				|| DeltaPlan.UpsertItemIndices.Num() != DeltaSourceCount)
			{
				Test.AddError(TEXT("Client delta benchmark produced an invalid plan"));
				return false;
			}
		}
		OutAverageMilliseconds =
			(FPlatformTime::Seconds() - StartSeconds)
			* 1000.0
			/ ReplicationBenchmarkIterations;
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFragmentSourceVolumeStateReplicationTest,
	"MatterFlux.Network.FragmentSourceVolumeStateReplicatesAtomically",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxFragmentSourceVolumeStateReplicationTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FGuid SourceId =
		FGuid::NewDeterministicGuid(TEXT("replicated.volume.source"), 1);
	FFragment2DSourceStreamingState State;
	State.Revision = 8;
	State.VolumeTopologyRevision = 5;
	State.VolumeFieldRevision = 7;
	State.VolumeEnvironmentEnergy = 100;
	State.SetRuntimeMask({ 1, 1 });
	State.VolumeCellStates.Add({
		FIntVector(1, 0, 0), TEXT("charcoal"), 42000 });

	FMatterFluxReplicatedFragmentSourceStateList States;
	const FMatterFluxFragmentSourceStateBatchUpdate Update{SourceId, &State};
	if (!TestEqual(TEXT("Valid Volume state commits"),
		States.UpsertAuthorityBatch(MakeArrayView(&Update, 1), 4, 4096),
		EMatterFluxFragmentSourceStateUpsertResult::Committed)
		|| !TestEqual(TEXT("One Source is published"), States.Items.Num(), 1))
	{
		return false;
	}
	const FMatterFluxReplicatedFragmentSourceState& Item = States.Items[0];
	TestEqual(TEXT("Topology revision is part of the atomic item"),
		Item.VolumeTopologyRevision, 5);
	TestEqual(TEXT("Field revision is part of the atomic item"),
		Item.VolumeFieldRevision, 7);
	TestEqual(TEXT("Environment energy is part of the atomic item"),
		Item.VolumeEnvironmentEnergy, static_cast<uint16>(100));
	if (!TestEqual(TEXT("Sparse Volume state is published"),
		Item.VolumeCellStates.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("Replicated cell address remains stable"),
		Item.VolumeCellStates[0].Cell, FIntVector(1, 0, 0));
	TestEqual(TEXT("Replicated cell material uses its stable name"),
		Item.VolumeCellStates[0].MaterialId, FName(TEXT("charcoal")));
	TestEqual(TEXT("Replicated cell energy remains exact"),
		Item.VolumeCellStates[0].Energy, static_cast<uint16>(42000));

	FFragment2DSourceStreamingState Duplicate = State;
	Duplicate.Revision = 9;
	const FFragment2DMaterialVolumeCellState DuplicateCell =
		Duplicate.VolumeCellStates[0];
	Duplicate.VolumeCellStates.Add(DuplicateCell);
	const FMatterFluxFragmentSourceStateBatchUpdate InvalidUpdate{
		SourceId, &Duplicate };
	TestEqual(TEXT("Duplicate Volume addresses reject the whole update"),
		States.UpsertAuthorityBatch(
			MakeArrayView(&InvalidUpdate, 1), 4, 4096),
		EMatterFluxFragmentSourceStateUpsertResult::InvalidState);
	TestEqual(TEXT("Rejected update leaves the prior revision untouched"),
		States.Items[0].Revision, 8);
	TestEqual(TEXT("Rejected update leaves the prior Volume payload untouched"),
		States.Items[0].VolumeCellStates.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFragmentSourceClientDeltaPlanTest,
	"MatterFlux.Network.FragmentSourceClientDeltaPlanIgnoresHistory",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxFragmentSourceClientDeltaPlanTest::RunTest(
	const FString& Parameters)
{
	constexpr int32 HistoricalSourceCount = 4096;
	FMatterFluxReplicatedFragmentSourceStateList States;
	States.Items.Reserve(HistoricalSourceCount);
	for (int32 SourceIndex = 0;
		SourceIndex < HistoricalSourceCount;
		++SourceIndex)
	{
		FMatterFluxReplicatedFragmentSourceState& Item =
			States.Items.AddDefaulted_GetRef();
		Item.SourceId = FGuid(
			static_cast<uint32>(SourceIndex + 1),
			0x9e3779b9u,
			0x85ebca6bu,
			0xc2b2ae35u);
		Item.ReplicationID = SourceIndex + 100;
		States.ItemMap.Add(Item.ReplicationID, SourceIndex);
	}

	FMatterFluxFragmentSourceClientApplyPlan InitialPlan;
	States.ConsumeClientApplyPlan(InitialPlan);
	TestTrue(
		TEXT("A newly received list first requests one complete rebuild"),
		InitialPlan.bFullRebuild);

	TArray<int32> ChangedIndices = {4095, 7};
	States.PostReplicatedChange(
		MakeArrayView(ChangedIndices),
		HistoricalSourceCount);
	FMatterFluxFragmentSourceClientApplyPlan DeltaPlan;
	States.ConsumeClientApplyPlan(DeltaPlan);
	TestFalse(
		TEXT("An ordinary Fast Array change remains incremental"),
		DeltaPlan.bFullRebuild);
	if (!TestEqual(
		TEXT("Only changed items enter the client plan"),
		DeltaPlan.UpsertItemIndices.Num(),
		2))
	{
		return false;
	}
	TestEqual(
		TEXT("Changed items are ordered by SourceId"),
		DeltaPlan.UpsertItemIndices[0],
		7);
	TestEqual(
		TEXT("History tail remains the second changed item"),
		DeltaPlan.UpsertItemIndices[1],
		4095);
	TestTrue(
		TEXT("A change plan contains no removals"),
		DeltaPlan.RemovedSourceIds.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFragmentSourceClientRemovalPlanTest,
	"MatterFlux.Network.FragmentSourceClientRemovalPlanIsStableAndUpsertWins",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxFragmentSourceClientRemovalPlanTest::RunTest(
	const FString& Parameters)
{
	FMatterFluxReplicatedFragmentSourceStateList States;
	for (int32 SourceIndex = 0; SourceIndex < 3; ++SourceIndex)
	{
		FMatterFluxReplicatedFragmentSourceState& Item =
			States.Items.AddDefaulted_GetRef();
		Item.SourceId = FGuid(
			static_cast<uint32>(SourceIndex + 1),
			0x9e3779b9u,
			0x85ebca6bu,
			0xc2b2ae35u);
		Item.ReplicationID = SourceIndex + 100;
		States.ItemMap.Add(Item.ReplicationID, SourceIndex);
	}
	FMatterFluxFragmentSourceClientApplyPlan InitialPlan;
	States.ConsumeClientApplyPlan(InitialPlan);

	TArray<int32> RemovedIndices = {2, 0, 2};
	States.PreReplicatedRemove(MakeArrayView(RemovedIndices), 1);
	FMatterFluxFragmentSourceClientApplyPlan RemovalPlan;
	States.ConsumeClientApplyPlan(RemovalPlan);
	if (!TestEqual(
		TEXT("Duplicate removals are collapsed"),
		RemovalPlan.RemovedSourceIds.Num(),
		2))
	{
		return false;
	}
	TestEqual(
		TEXT("Removed Source IDs are stable"),
		RemovalPlan.RemovedSourceIds[0],
		States.Items[0].SourceId);
	TestEqual(
		TEXT("The second removal remains stable"),
		RemovalPlan.RemovedSourceIds[1],
		States.Items[2].SourceId);

	TArray<int32> ReplacedIndex = {1};
	States.PreReplicatedRemove(MakeArrayView(ReplacedIndex), 2);
	States.PostReplicatedAdd(MakeArrayView(ReplacedIndex), 3);
	FMatterFluxFragmentSourceClientApplyPlan ReplacedPlan;
	States.ConsumeClientApplyPlan(ReplacedPlan);
	TestEqual(
		TEXT("A remove/re-add applies the final item"),
		ReplacedPlan.UpsertItemIndices.Num(),
		1);
	TestTrue(
		TEXT("The final upsert supersedes the queued removal"),
		ReplacedPlan.RemovedSourceIds.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFragmentSourceClientPlanFallbackTest,
	"MatterFlux.Network.FragmentSourceClientPlanFallsBackOnReplicationMismatch",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxFragmentSourceClientPlanFallbackTest::RunTest(
	const FString& Parameters)
{
	FMatterFluxReplicatedFragmentSourceStateList States;
	FMatterFluxReplicatedFragmentSourceState& Item =
		States.Items.AddDefaulted_GetRef();
	Item.SourceId = FGuid(1, 2, 3, 4);
	Item.ReplicationID = 100;
	States.ItemMap.Add(Item.ReplicationID, 0);
	FMatterFluxFragmentSourceClientApplyPlan InitialPlan;
	States.ConsumeClientApplyPlan(InitialPlan);

	TArray<int32> ChangedIndex = {0};
	States.PostReplicatedChange(MakeArrayView(ChangedIndex), 1);
	States.ItemMap.Reset();
	FMatterFluxFragmentSourceClientApplyPlan FallbackPlan;
	States.ConsumeClientApplyPlan(FallbackPlan);
	TestTrue(
		TEXT("A stale ReplicationID requests a complete rebuild"),
		FallbackPlan.bFullRebuild);
	TestTrue(
		TEXT("A failed delta never exposes a partial upsert plan"),
		FallbackPlan.UpsertItemIndices.IsEmpty());
	TestTrue(
		TEXT("A failed delta never exposes partial removals"),
		FallbackPlan.RemovedSourceIds.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFragmentSourceClientDeltaPerformanceTest,
	"MatterFlux.Performance.FragmentSourceClientDeltaPlansIgnoreFourThousandHistoryItems",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::PerfFilter)

bool FMatterFluxFragmentSourceClientDeltaPerformanceTest::RunTest(
	const FString& Parameters)
{
	double OneSourceMilliseconds = 0.0;
	double SixteenSourceMilliseconds = 0.0;
	double SixtyFourSourceMilliseconds = 0.0;
	if (!BenchmarkClientDeltaPlan(*this, 1, OneSourceMilliseconds)
		|| !BenchmarkClientDeltaPlan(*this, 16, SixteenSourceMilliseconds)
		|| !BenchmarkClientDeltaPlan(*this, 64, SixtyFourSourceMilliseconds))
	{
		return false;
	}
	AddInfo(FString::Printf(
		TEXT("Client delta apply plan with 4096 historical Sources: 1=%.3f ms, 16=%.3f ms, 64=%.3f ms"),
		OneSourceMilliseconds,
		SixteenSourceMilliseconds,
		SixtyFourSourceMilliseconds));
	TestTrue(
		TEXT("One changed Source plan remains sub-millisecond"),
		OneSourceMilliseconds < 1.0);
	TestTrue(
		TEXT("Sixteen changed Source plan remains sub-millisecond"),
		SixteenSourceMilliseconds < 1.0);
	TestTrue(
		TEXT("Sixty-four changed Source plan remains sub-millisecond"),
		SixtyFourSourceMilliseconds < 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFragmentSourceStateBatchPerformanceTest,
	"MatterFlux.Performance.FragmentSourceStateBatchEncodesOneSixteenAndSixtyFourSources",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::PerfFilter)

bool FMatterFluxFragmentSourceStateBatchPerformanceTest::RunTest(
	const FString& Parameters)
{
	double OneSourceMilliseconds = 0.0;
	double SixteenSourceMilliseconds = 0.0;
	double SixtyFourSourceMilliseconds = 0.0;
	if (!BenchmarkSourceBatch(*this, 1, OneSourceMilliseconds)
		|| !BenchmarkSourceBatch(*this, 16, SixteenSourceMilliseconds)
		|| !BenchmarkSourceBatch(*this, 64, SixtyFourSourceMilliseconds))
	{
		return false;
	}
	AddInfo(FString::Printf(
		TEXT("Source replication batch average: 1=%.3f ms, 16=%.3f ms, 64=%.3f ms"),
		OneSourceMilliseconds,
		SixteenSourceMilliseconds,
		SixtyFourSourceMilliseconds));
	TestTrue(
		TEXT("One Source encoding remains sub-millisecond"),
		OneSourceMilliseconds < 1.0);
	TestTrue(
		TEXT("Sixteen Source encoding remains below three milliseconds"),
		SixteenSourceMilliseconds < 3.0);
	TestTrue(
		TEXT("Sixty-four Source encoding remains below ten milliseconds"),
		SixtyFourSourceMilliseconds < 10.0);
	return true;
}
