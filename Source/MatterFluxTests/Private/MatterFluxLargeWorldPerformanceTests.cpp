#include "Fragment/Fragment2DSourceActor.h"
#include "Game/MatterFluxCharacter.h"
#include "Game/MatterFluxPlayableLevel.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Material/MatterFluxGroundCombustionRuntime.h"
#include "Material/MatterFluxLogicalSourceCombustionIndex.h"

#include "Algo/Sort.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLogicalSourceHistoryPerformanceTest,
	"MatterFlux.Performance.LogicalSourceActiveWorkIgnoresExtinguishedHistory",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::PerfFilter)

bool FMatterFluxLogicalSourceHistoryPerformanceTest::RunTest(
	const FString& Parameters)
{
	constexpr int32 HistoricalSourceCount = 65536;
	constexpr int32 ActiveSourceCount = 32;
	constexpr int32 VisualRefreshCount = 10000;
	const TArray<uint8> ExtinguishedMask = {0, 0, 0, 0};
	const TArray<uint8> BurningMask = {0, 1, 0, 0};
	MatterFlux::Combustion::FLogicalSourceCombustionIndex Index;

	for (int32 SourceIndex = 0;
		SourceIndex < HistoricalSourceCount;
		++SourceIndex)
	{
		const FGuid SourceId(
			static_cast<uint32>(SourceIndex + 1),
			0x9e3779b9u,
			0x85ebca6bu,
			0xc2b2ae35u);
		if (!Index.ApplySnapshot(SourceId, true, ExtinguishedMask))
		{
			AddError(TEXT("Historical Source snapshot was rejected"));
			return false;
		}
	}
	for (int32 SourceIndex = 0;
		SourceIndex < ActiveSourceCount;
		++SourceIndex)
	{
		const FGuid SourceId(
			static_cast<uint32>(HistoricalSourceCount + SourceIndex + 1),
			0x27d4eb2du,
			0x165667b1u,
			0xd3a2646cu);
		Index.ApplySnapshot(SourceId, true, BurningMask);
	}
	TestEqual(
		TEXT("Only current fires survive a long exploration history"),
		Index.Num(),
		ActiveSourceCount);

	TArray<FGuid> ActiveIds;
	const double StartSeconds = FPlatformTime::Seconds();
	for (int32 RefreshIndex = 0;
		RefreshIndex < VisualRefreshCount;
		++RefreshIndex)
	{
		Index.GatherStableIds(ActiveIds);
	}
	const double ElapsedMilliseconds =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	AddInfo(FString::Printf(
		TEXT("Logical Source active work: %d visual refreshes with %d active after %d extinguished history entries in %.2f ms"),
		VisualRefreshCount,
		ActiveSourceCount,
		HistoricalSourceCount,
		ElapsedMilliseconds));
	TestEqual(
		TEXT("Every refresh returns only the bounded active set"),
		ActiveIds.Num(),
		ActiveSourceCount);
	TestTrue(
		TEXT("Visual work remains bounded by current fire, not world history"),
		ElapsedMilliseconds < 100.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxGroundRuntimeSparseUpdatePerformanceTest,
	"MatterFlux.Performance.GroundRuntimeSparseUpdatesDoNotCopyWholeMask",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::PerfFilter)

bool FMatterFluxGroundRuntimeSparseUpdatePerformanceTest::RunTest(
	const FString& Parameters)
{
	constexpr int32 Width = MatterFlux::PlayableLevel::TerrainCellsX;
	constexpr int32 Height = MatterFlux::PlayableLevel::TerrainCellsY;
	constexpr int32 IgnitionCount = 8192;
	FFragmentSourceMask Mask;
	Mask.Width = Width;
	Mask.Height = Height;
	Mask.CellSize = MatterFlux::PlayableLevel::TerrainCellSize;
	Mask.SolidMask.Init(1, Width * Height);

	FMatterFluxCombustionDefinition Rule;
	Rule.Id = TEXT("ground_sparse_update_performance");
	Rule.FuelMaterial = TEXT("grassland");
	Rule.FlameMaterial = TEXT("fire");
	Rule.SmokeMaterial = TEXT("smoke");
	Rule.ResidueMaterial = TEXT("ash");
	Rule.IgnitionChancePermille = 1000;
	Rule.SpreadChancePermille = 0;
	Rule.BurnDurationSteps = 255;

	MatterFlux::Combustion::FGroundRuntimeSettings Settings;
	Settings.Width = Width;
	Settings.Height = Height;
	MatterFlux::Combustion::FGroundCombustionRuntime Runtime;
	FString Error;
	if (!TestTrue(
		TEXT("Large sparse-update runtime initializes"),
		Runtime.Initialize(Settings, Mask, Rule, 778899, Error)))
	{
		AddError(Error);
		return false;
	}

	int32 Ignited = 0;
	const double StartSeconds = FPlatformTime::Seconds();
	for (int32 Index = 0; Index < IgnitionCount; ++Index)
	{
		Ignited += Runtime.Ignite(
			FIntPoint(Index % Width, Index / Width),
			Rule.FlameMaterial)
			? 1
			: 0;
	}
	const double ElapsedMilliseconds =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	AddInfo(FString::Printf(
		TEXT("GroundRuntime %d sparse ignitions across %dx%d cells: %.2f ms"),
		IgnitionCount,
		Width,
		Height,
		ElapsedMilliseconds));
	TestEqual(
		TEXT("Every distinct fuel cell ignites"),
		Ignited,
		IgnitionCount);
	TestTrue(
		TEXT("Sparse ignition cost stays independent of the full mask size"),
		ElapsedMilliseconds < 20.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFragmentSourceStateBatchDeterminismTest,
	"MatterFlux.Network.FragmentSourceStateBatchCommitsDeterministically",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxFragmentSourceStateBatchDeterminismTest::RunTest(
	const FString& Parameters)
{
	FMatterFluxReplicatedFragmentSourceStateList States;
	FFragment2DSourceStreamingState FirstState;
	FirstState.Revision = 11;
	FirstState.SetRuntimeMask({1, 0, 1, 0, 1, 0, 1, 0});
	FFragment2DSourceStreamingState SecondState;
	SecondState.Revision = 12;
	SecondState.SetRuntimeMask({0, 1, 0, 1, 0, 1, 0, 1});
	FFragment2DSourceStreamingState ThirdState;
	ThirdState.Revision = 13;
	ThirdState.SetRuntimeMask({1, 1, 0, 0, 1, 1, 0, 0});

	const FGuid FirstId(1, 0, 0, 0);
	const FGuid SecondId(2, 0, 0, 0);
	const FGuid ThirdId(3, 0, 0, 0);
	const TArray<FMatterFluxFragmentSourceStateBatchUpdate> Updates =
	{
		{ThirdId, &ThirdState},
		{FirstId, &FirstState},
		{SecondId, &SecondState}
	};
	TestEqual(
		TEXT("A valid Source batch commits as one transaction"),
		States.UpsertAuthorityBatch(Updates, 3, 3),
		EMatterFluxFragmentSourceStateUpsertResult::Committed);
	if (!TestEqual(TEXT("Every batch member is present"), States.Items.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("First Source is ordered deterministically"), States.Items[0].SourceId, FirstId);
	TestEqual(TEXT("Second Source is ordered deterministically"), States.Items[1].SourceId, SecondId);
	TestEqual(TEXT("Third Source is ordered deterministically"), States.Items[2].SourceId, ThirdId);
	TestEqual(TEXT("First Source mask is packed"), States.Items[0].PackedRuntimeMask, TArray<uint8>({0x55}));
	TestEqual(TEXT("Second Source mask is packed"), States.Items[1].PackedRuntimeMask, TArray<uint8>({0xaa}));
	TestEqual(TEXT("Third Source mask is packed"), States.Items[2].PackedRuntimeMask, TArray<uint8>({0x33}));
	TestEqual(TEXT("The committed byte count includes the entire batch"), States.GetAuthorityPayloadByteCount(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFragmentSourceStateBatchAtomicityTest,
	"MatterFlux.Network.FragmentSourceStateBatchRejectsAtomically",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxFragmentSourceStateBatchAtomicityTest::RunTest(
	const FString& Parameters)
{
	FMatterFluxReplicatedFragmentSourceStateList States;
	const FGuid FirstId(1, 0, 0, 0);
	const FGuid SecondId(2, 0, 0, 0);
	FFragment2DSourceStreamingState FirstInitial;
	FirstInitial.Revision = 3;
	FirstInitial.SetRuntimeMask({1, 0, 0, 0, 0, 0, 0, 0});
	FFragment2DSourceStreamingState SecondInitial;
	SecondInitial.Revision = 4;
	SecondInitial.SetRuntimeMask({0, 1, 0, 0, 0, 0, 0, 0});
	const TArray<FMatterFluxFragmentSourceStateBatchUpdate> InitialUpdates =
	{
		{FirstId, &FirstInitial},
		{SecondId, &SecondInitial}
	};
	if (!TestEqual(
		TEXT("Initial Source batch commits"),
		States.UpsertAuthorityBatch(InitialUpdates, 2, 2),
		EMatterFluxFragmentSourceStateUpsertResult::Committed))
	{
		return false;
	}

	FFragment2DSourceStreamingState ValidChange;
	ValidChange.Revision = 30;
	ValidChange.SetRuntimeMask({0, 0, 1, 0, 0, 0, 0, 0});
	FFragment2DSourceStreamingState InvalidChange;
	InvalidChange.Revision = 40;
	InvalidChange.SetRuntimeMask({0, 0, 0, 2, 0, 0, 0, 0});
	const TArray<FMatterFluxFragmentSourceStateBatchUpdate> RejectedUpdates =
	{
		{FirstId, &ValidChange},
		{SecondId, &InvalidChange}
	};
	TestEqual(
		TEXT("One malformed member rejects the whole batch"),
		States.UpsertAuthorityBatch(RejectedUpdates, 2, 2),
		EMatterFluxFragmentSourceStateUpsertResult::InvalidState);
	if (!TestEqual(TEXT("Rejected batch preserves item count"), States.Items.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("Earlier valid member keeps its old revision"), States.Items[0].Revision, 3);
	TestEqual(TEXT("Earlier valid member keeps its old payload"), States.Items[0].PackedRuntimeMask, TArray<uint8>({0x01}));
	TestEqual(TEXT("Malformed member keeps its old revision"), States.Items[1].Revision, 4);
	TestEqual(TEXT("Malformed member keeps its old payload"), States.Items[1].PackedRuntimeMask, TArray<uint8>({0x02}));
	TestEqual(TEXT("Rejected batch preserves byte accounting"), States.GetAuthorityPayloadByteCount(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFragmentSourceStateBatchScalePerformanceTest,
	"MatterFlux.Performance.FragmentSourceStateBatchScalesWithUpdates",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::PerfFilter)

bool FMatterFluxFragmentSourceStateBatchScalePerformanceTest::RunTest(
	const FString& Parameters)
{
	constexpr int32 SourceCount = 4096;
	constexpr int32 UpdateCount = SourceCount * 2;
	constexpr int32 MaximumPayloadBytes = 1024 * 1024;
	FMatterFluxReplicatedFragmentSourceStateList States;
	TArray<FGuid> SourceIds;
	SourceIds.Reserve(SourceCount);
	auto MakeReplicatedLogicalState = [](
		const int32 Revision,
		const int32 Pattern)
	{
		FFragment2DSourceStreamingState State;
		State.Revision = Revision;
		State.bHasCombustionState = true;
		TArray<uint8> RuntimeMask;
		RuntimeMask.SetNumUninitialized(256);
		State.CombustionState.ResidueMask.SetNumUninitialized(256);
		State.CombustionState.BurningMask.SetNumUninitialized(256);
		for (int32 CellIndex = 0; CellIndex < 256; ++CellIndex)
		{
			RuntimeMask[CellIndex] =
				((CellIndex + Pattern) & 1) != 0 ? 1 : 0;
			State.CombustionState.ResidueMask[CellIndex] =
				((CellIndex + Pattern) % 5) == 0 ? 1 : 0;
			State.CombustionState.BurningMask[CellIndex] =
				((CellIndex + Pattern) % 7) == 0 ? 4 : 0;
		}
		State.SetRuntimeMask(MoveTemp(RuntimeMask));
		return State;
	};
	for (int32 Index = 0; Index < SourceCount; ++Index)
	{
		const FGuid SourceId(
			static_cast<uint32>(Index + 1),
			0x9e3779b9u ^ static_cast<uint32>(Index),
			0x85ebca6bu + static_cast<uint32>(Index * 17),
			0xc2b2ae35u ^ static_cast<uint32>(Index * 31));
		SourceIds.Add(SourceId);
		FFragment2DSourceStreamingState State =
			MakeReplicatedLogicalState(1, Index);
		const FMatterFluxFragmentSourceStateBatchUpdate Update{
			SourceId,
			&State};
		if (!TestEqual(
			TEXT("Initial replicated Source state commits"),
			States.UpsertAuthorityBatch(
				MakeArrayView(&Update, 1),
				SourceCount,
				MaximumPayloadBytes),
			EMatterFluxFragmentSourceStateUpsertResult::Committed))
		{
			return false;
		}
	}

	const double StartSeconds = FPlatformTime::Seconds();
	for (int32 UpdateIndex = 0;
		UpdateIndex < UpdateCount;
		++UpdateIndex)
	{
		const int32 SourceIndex = UpdateIndex % SourceCount;
		FFragment2DSourceStreamingState State =
			MakeReplicatedLogicalState(
				2 + UpdateIndex / SourceCount,
				UpdateIndex);
		const FMatterFluxFragmentSourceStateBatchUpdate Update{
			SourceIds[SourceIndex],
			&State};
		if (States.UpsertAuthorityBatch(
			MakeArrayView(&Update, 1),
			SourceCount,
			MaximumPayloadBytes)
			!= EMatterFluxFragmentSourceStateUpsertResult::Committed)
		{
			AddError(TEXT("Existing replicated Source update was rejected"));
			return false;
		}
	}
	const double ElapsedMilliseconds =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	AddInfo(FString::Printf(
		TEXT("Fragment Source batch store: %d updates across %d entries in %.2f ms"),
		UpdateCount,
		SourceCount,
		ElapsedMilliseconds));
	TestEqual(
		TEXT("Updates replace existing Source states without duplicates"),
		States.Items.Num(),
		SourceCount);
	TestEqual(
		TEXT("Cached payload byte count remains exact"),
		States.GetAuthorityPayloadByteCount(),
		SourceCount * 96);
	TestTrue(
		TEXT("Source state upsert cost scales with updates, not list length"),
		ElapsedMilliseconds < 30.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFragmentSourceStateBudgetTransactionTest,
	"MatterFlux.Network.FragmentSourceStateBudgetsAreAtomic",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxFragmentSourceStateBudgetTransactionTest::RunTest(
	const FString& Parameters)
{
	FMatterFluxReplicatedFragmentSourceStateList States;
	const FGuid FirstSourceId(1, 2, 3, 4);
	FFragment2DSourceStreamingState Initial;
	Initial.Revision = 7;
	Initial.bHasCombustionState = true;
	Initial.CombustionState.ResidueMask.Init(0, 16);
	Initial.CombustionState.ResidueMask[1] = 1;
	Initial.CombustionState.BurningMask.Init(0, 16);
	Initial.CombustionState.BurningMask[2] = 5;
	TArray<uint8> InitialRuntimeMask;
	InitialRuntimeMask.Init(0, 16);
	InitialRuntimeMask[0] = 1;
	Initial.SetRuntimeMask(MoveTemp(InitialRuntimeMask));
	const FMatterFluxFragmentSourceStateBatchUpdate InitialUpdate{
		FirstSourceId,
		&Initial};
	TestEqual(
		TEXT("Initial state commits at both budget limits"),
		States.UpsertAuthorityBatch(
			MakeArrayView(&InitialUpdate, 1),
			1,
			6),
		EMatterFluxFragmentSourceStateUpsertResult::Committed);

	FFragment2DSourceStreamingState Extra;
	Extra.Revision = 1;
	Extra.SetRuntimeMask({1, 0, 0, 1, 0, 0, 1, 0});
	const FMatterFluxFragmentSourceStateBatchUpdate ExtraUpdate{
		FGuid(5, 6, 7, 8),
		&Extra};
	TestEqual(
		TEXT("A distinct state over the item budget is rejected"),
		States.UpsertAuthorityBatch(
			MakeArrayView(&ExtraUpdate, 1),
			1,
			6),
		EMatterFluxFragmentSourceStateUpsertResult::ItemBudgetExceeded);

	FFragment2DSourceStreamingState Oversized;
	Oversized.Revision = 8;
	TArray<uint8> OversizedMask;
	OversizedMask.Init(1, 56);
	Oversized.SetRuntimeMask(MoveTemp(OversizedMask));
	const FMatterFluxFragmentSourceStateBatchUpdate OversizedUpdate{
		FirstSourceId,
		&Oversized};
	TestEqual(
		TEXT("An update over the byte budget is rejected"),
		States.UpsertAuthorityBatch(
			MakeArrayView(&OversizedUpdate, 1),
			1,
			6),
		EMatterFluxFragmentSourceStateUpsertResult::ByteBudgetExceeded);

	FFragment2DSourceStreamingState Invalid;
	Invalid.Revision = 9;
	Invalid.SetRuntimeMask({1, 0, 0, 0, 0, 0, 0, 0});
	const FMatterFluxFragmentSourceStateBatchUpdate InvalidUpdate{
		FGuid(),
		&Invalid};
	TestEqual(
		TEXT("An invalid SourceId is rejected"),
		States.UpsertAuthorityBatch(
			MakeArrayView(&InvalidUpdate, 1),
			1,
			6),
		EMatterFluxFragmentSourceStateUpsertResult::InvalidState);

	TestEqual(
		TEXT("Rejected mutations do not append entries"),
		States.Items.Num(),
		1);
	TestEqual(
		TEXT("Rejected mutations preserve the committed revision"),
		States.Items[0].Revision,
		7);
	TestEqual(
		TEXT("Rejected mutations preserve the committed payload"),
		States.Items[0].PackedRuntimeMask,
		TArray<uint8>({1, 0}));
	TestEqual(
		TEXT("Rejected mutations preserve the cached byte count"),
		States.GetAuthorityPayloadByteCount(),
		6);

	States.ResetAuthorityItems();
	TestTrue(TEXT("Reset removes all replicated Source states"), States.Items.IsEmpty());
	TestEqual(
		TEXT("Reset clears the cached byte count"),
		States.GetAuthorityPayloadByteCount(),
		0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxGroundRuntimeAdvanceBreakdownPerformanceTest,
	"MatterFlux.Performance.GroundRuntimeAdvanceAndReplicationBreakdown",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::PerfFilter)

bool FMatterFluxGroundRuntimeAdvanceBreakdownPerformanceTest::RunTest(
	const FString& Parameters)
{
	constexpr int32 Width = MatterFlux::PlayableLevel::TerrainCellsX;
	constexpr int32 Height = MatterFlux::PlayableLevel::TerrainCellsY;
	FFragmentSourceMask Mask;
	Mask.Width = Width;
	Mask.Height = Height;
	Mask.CellSize = MatterFlux::PlayableLevel::TerrainCellSize;
	Mask.SolidMask.Init(1, Width * Height);

	// Keep this aligned with Content/Lua/World/Chemistry.lua so the benchmark
	// represents the actual grassland fire rather than a synthetic workload.
	FMatterFluxCombustionDefinition Rule;
	Rule.Id = TEXT("grassland_burn_performance");
	Rule.FuelMaterial = TEXT("grassland");
	Rule.FlameMaterial = TEXT("fire");
	Rule.SmokeMaterial = TEXT("smoke");
	Rule.ResidueMaterial = TEXT("ash");
	Rule.IgnitionChancePermille = 1000;
	Rule.SpreadChancePermille = 420;
	Rule.BurnDurationSteps = 8;

	MatterFlux::Combustion::FGroundRuntimeSettings Settings;
	Settings.Width = Width;
	Settings.Height = Height;
	MatterFlux::Combustion::FGroundCombustionRuntime Runtime;
	FString Error;
	if (!TestTrue(
		TEXT("Ground breakdown runtime initializes"),
		Runtime.Initialize(Settings, Mask, Rule, 24681357, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(
		TEXT("Ground breakdown runtime ignites its center cell"),
		Runtime.Ignite(FIntPoint(Width / 2, Height / 2), Rule.FlameMaterial));

	double AdvanceSeconds = 0.0;
	double ReplicationSeconds = 0.0;
	int32 PublishedChunks = 0;
	for (int32 Step = 0; Step < 120; ++Step)
	{
		const double AdvanceStart = FPlatformTime::Seconds();
		Runtime.AdvanceAuthority(0.1f);
		AdvanceSeconds += FPlatformTime::Seconds() - AdvanceStart;
		if (Runtime.HasPendingReplication())
		{
			TArray<FMatterFluxGroundStateChunk> Batch;
			const double ReplicationStart = FPlatformTime::Seconds();
			if (!Runtime.BuildPendingReplication(Batch, Error))
			{
				AddError(Error);
				return false;
			}
			ReplicationSeconds +=
				FPlatformTime::Seconds() - ReplicationStart;
			PublishedChunks += Batch.Num();
		}
	}
	const double AdvanceMilliseconds = AdvanceSeconds * 1000.0;
	const double ReplicationMilliseconds = ReplicationSeconds * 1000.0;
	AddInfo(FString::Printf(
		TEXT("GroundRuntime 120 gameplay fire steps: advance %.2f ms, replication %.2f ms, published chunks=%d, residue=%d"),
		AdvanceMilliseconds,
		ReplicationMilliseconds,
		PublishedChunks,
		Runtime.CountResidueCells()));
	TestTrue(
		TEXT("Sparse ground simulation remains below the aggregate frame budget"),
		AdvanceMilliseconds < 100.0);
	TestTrue(
		TEXT("Dirty-chunk encoding remains below the aggregate frame budget"),
		ReplicationMilliseconds < 100.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLargeWorldStreamingPerformanceTest,
	"MatterFlux.Performance.LargeWorldStreamingMovementAndCombustion",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::PerfFilter)

bool FMatterFluxLargeWorldStreamingPerformanceTest::RunTest(
	const FString& Parameters)
{
	constexpr int32 ChunkSize = 64;
	constexpr int32 ExpectedActiveDiameter = 3;
	TestTrue(
		TEXT("The random map is much wider than the active chunk window"),
		MatterFlux::PlayableLevel::TerrainCellsX
			>= ChunkSize * ExpectedActiveDiameter * 2);
	TestTrue(
		TEXT("The random map is much taller than the active chunk window"),
		MatterFlux::PlayableLevel::TerrainCellsY
			>= ChunkSize * ExpectedActiveDiameter * 2);

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor =
		World
			? World->SpawnActor<AMatterFluxPlayableWorldActor>()
			: nullptr;
	if (!TestNotNull(TEXT("Large-world actor spawns"), WorldActor))
	{
		return false;
	}

	const double GenerationStart = FPlatformTime::Seconds();
	WorldActor->Regenerate(24681357);
	const double GenerationMilliseconds =
		(FPlatformTime::Seconds() - GenerationStart) * 1000.0;
	AddInfo(FString::Printf(
		TEXT("LargeWorld generation: %.2f ms, resident=%d archived=%d"),
		GenerationMilliseconds,
		WorldActor->GetResidentMaterialChunkCount(),
		WorldActor->GetArchivedMaterialChunkCount()));
	TestTrue(
		TEXT("Large-world generation stays below the startup hitch budget"),
		GenerationMilliseconds < 5000.0);
	TestTrue(
		TEXT("Resident simulation chunks remain bounded by 3x3"),
		WorldActor->GetResidentMaterialChunkCount()
			<= ExpectedActiveDiameter * ExpectedActiveDiameter);
	TestTrue(
		TEXT("Terrain beyond the active window is archived"),
		WorldActor->GetArchivedMaterialChunkCount()
			> WorldActor->GetResidentMaterialChunkCount());
	TestTrue(
		TEXT("Visible terrain triangles are bounded by streamed chunks"),
		WorldActor->GetVisibleTerrainTriangleCount() < 3000000);
	TestTrue(
		TEXT("Most decoration masks stay cached outside the loaded window"),
		WorldActor->GetCachedFragmentSourceCount()
			> WorldActor->GetGeneratedFragmentSourceCount() * 4);
	TestTrue(
		TEXT("Nearby decoration actor count remains bounded"),
		WorldActor->GetGeneratedFragmentSourceCount() < 500);

	// A local spell query must not scale with the total number of cached
	// decorations. Keep the bounds far outside the generated map so this probes
	// broad-phase cost without changing combustion state or materializing Actors.
	constexpr int32 FarIgnitionQueryCount = 65536;
	const FBox FarIgnitionBounds(
		FVector(10000000.0, 10000000.0, -50.0),
		FVector(10000100.0, 10000100.0, 50.0));
	int32 UnexpectedFarIgnitions = 0;
	const double FarIgnitionStart = FPlatformTime::Seconds();
	for (int32 QueryIndex = 0;
		QueryIndex < FarIgnitionQueryCount;
		++QueryIndex)
	{
		UnexpectedFarIgnitions +=
			WorldActor->IgniteLogicalFragmentSourcesInBounds(
				FarIgnitionBounds,
				FarIgnitionBounds.GetCenter(),
				TEXT("fire"),
				900000 + QueryIndex);
	}
	const double FarIgnitionMilliseconds =
		(FPlatformTime::Seconds() - FarIgnitionStart) * 1000.0;
	AddInfo(FString::Printf(
		TEXT("LargeWorld %d far local ignition queries: %.2f ms"),
		FarIgnitionQueryCount,
		FarIgnitionMilliseconds));
	TestEqual(
		TEXT("Queries outside the map never ignite a source"),
		UnexpectedFarIgnitions,
		0);
	TestTrue(
		TEXT("Local spell broad phase remains independent of cached world size"),
		FarIgnitionMilliseconds < 500.0);

	FActorSpawnParameters PlayerSpawnParameters;
	PlayerSpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APlayerController* PlayerController =
		World->SpawnActor<APlayerController>(
			APlayerController::StaticClass(),
			FTransform::Identity,
			PlayerSpawnParameters);
	AMatterFluxCharacter* PlayerCharacter =
		World->SpawnActor<AMatterFluxCharacter>(
			AMatterFluxCharacter::StaticClass(),
			FTransform::Identity,
			PlayerSpawnParameters);
	if (!TestNotNull(
			TEXT("Performance test player controller spawns"),
			PlayerController)
		|| !TestNotNull(
			TEXT("Performance test character spawns"),
			PlayerCharacter))
	{
		return false;
	}
	PlayerController->Possess(PlayerCharacter);

	constexpr float FixedDeltaSeconds = 1.0f / 60.0f;
	const float HalfPathX =
		MatterFlux::PlayableLevel::TerrainCellsX
			* MatterFlux::PlayableLevel::TerrainCellSize * 0.35f;
	const float HalfPathY =
		MatterFlux::PlayableLevel::TerrainCellsY
			* MatterFlux::PlayableLevel::TerrainCellSize * 0.35f;
	const FVector PathStart(-HalfPathX, -HalfPathY, 0.0f);
	const FVector PathEnd(HalfPathX, HalfPathY, 0.0f);
	const float PathLength = FVector::Distance(PathStart, PathEnd);
	const auto ToChunk = [](const FVector& Location)
	{
		const FIntPoint Cell(
			FMath::RoundToInt(
				Location.X
					/ MatterFlux::PlayableLevel::TerrainCellSize),
			FMath::RoundToInt(
				Location.Y
					/ MatterFlux::PlayableLevel::TerrainCellSize));
		return FIntPoint(
			FMath::FloorToInt(
				static_cast<double>(Cell.X) / ChunkSize),
			FMath::FloorToInt(
				static_cast<double>(Cell.Y) / ChunkSize));
	};

	struct FMovementProfile
	{
		const TCHAR* Name;
		float SpeedCentimetersPerSecond;
		bool bReverse;
	};
	const FMovementProfile MovementProfiles[] =
	{
		{ TEXT("walk"), 500.0f, false },
		{ TEXT("sprint"), 1200.0f, true },
		{ TEXT("high-speed"), 2500.0f, false }
	};
	for (const FMovementProfile& Profile : MovementProfiles)
	{
		const FVector ProfileStart =
			Profile.bReverse ? PathEnd : PathStart;
		const FVector ProfileEnd =
			Profile.bReverse ? PathStart : PathEnd;
		PlayerCharacter->SetActorLocation(ProfileStart);
		WorldActor->Tick(0.0f);
		for (int32 WarmupFrame = 0;
			WorldActor->GetPendingFragmentSourceSpawnCount() > 0
				&& WarmupFrame < 120;
			++WarmupFrame)
		{
			WorldActor->Tick(FixedDeltaSeconds);
		}

		const int32 MovementFrames = FMath::CeilToInt(
			PathLength
				/ (Profile.SpeedCentimetersPerSecond
					* FixedDeltaSeconds));
		double TotalMovementMilliseconds = 0.0;
		double MaximumMovementFrameMilliseconds = 0.0;
		double MaximumBoundaryFrameMilliseconds = 0.0;
		double TotalWorldTickMilliseconds = 0.0;
		double MaximumWorldTickMilliseconds = 0.0;
		double MaximumBoundaryWorldTickMilliseconds = 0.0;
		TArray<double> MovementFrameSamples;
		MovementFrameSamples.Reserve(MovementFrames);
		int32 BoundaryCrossingCount = 0;
		FIntPoint PreviousChunk = ToChunk(ProfileStart);
		for (int32 Frame = 1; Frame <= MovementFrames; ++Frame)
		{
			const FVector Location = FMath::Lerp(
				ProfileStart,
				ProfileEnd,
				static_cast<float>(Frame)
					/ static_cast<float>(MovementFrames));
			const FIntPoint NewChunk = ToChunk(Location);
			const bool bCrossedBoundary = NewChunk != PreviousChunk;
			const double FrameStart = FPlatformTime::Seconds();
			PlayerCharacter->SetActorLocation(Location);
			const double TickStart = FPlatformTime::Seconds();
			WorldActor->Tick(FixedDeltaSeconds);
			const double TickMilliseconds =
				(FPlatformTime::Seconds() - TickStart) * 1000.0;
			const double FrameMilliseconds =
				(FPlatformTime::Seconds() - FrameStart) * 1000.0;
			MovementFrameSamples.Add(FrameMilliseconds);
			TotalMovementMilliseconds += FrameMilliseconds;
			TotalWorldTickMilliseconds += TickMilliseconds;
			MaximumMovementFrameMilliseconds = FMath::Max(
				MaximumMovementFrameMilliseconds,
				FrameMilliseconds);
			MaximumWorldTickMilliseconds = FMath::Max(
				MaximumWorldTickMilliseconds,
				TickMilliseconds);
			if (bCrossedBoundary)
			{
				++BoundaryCrossingCount;
				MaximumBoundaryFrameMilliseconds = FMath::Max(
					MaximumBoundaryFrameMilliseconds,
					FrameMilliseconds);
				MaximumBoundaryWorldTickMilliseconds = FMath::Max(
					MaximumBoundaryWorldTickMilliseconds,
					TickMilliseconds);
			}
			PreviousChunk = NewChunk;
		}
		Algo::Sort(MovementFrameSamples);
		const int32 P95Index = FMath::Clamp(
			FMath::CeilToInt(
				static_cast<double>(MovementFrameSamples.Num())
					* 0.95)
				- 1,
			0,
			MovementFrameSamples.Num() - 1);
		const double P95MovementFrameMilliseconds =
			MovementFrameSamples[P95Index];
		AddInfo(FString::Printf(
			TEXT("LargeWorld player %s %.0f cm/s: %d frames, %d boundaries; frame average %.3f ms, p95 %.2f ms, max %.2f ms, boundary max %.2f ms; tick total %.2f ms, max %.2f ms, boundary max %.2f ms"),
			Profile.Name,
			Profile.SpeedCentimetersPerSecond,
			MovementFrames,
			BoundaryCrossingCount,
			TotalMovementMilliseconds
				/ static_cast<double>(MovementFrames),
			P95MovementFrameMilliseconds,
			MaximumMovementFrameMilliseconds,
			MaximumBoundaryFrameMilliseconds,
			TotalWorldTickMilliseconds,
			MaximumWorldTickMilliseconds,
			MaximumBoundaryWorldTickMilliseconds));
		TestTrue(
			*FString::Printf(
				TEXT("%s path crosses multiple streaming chunk boundaries"),
				Profile.Name),
			BoundaryCrossingCount >= 4);
		TestTrue(
			*FString::Printf(
				TEXT("%s chunk-boundary world ticks stay inside one 60 FPS frame"),
				Profile.Name),
			MaximumBoundaryWorldTickMilliseconds < 16.67);
		TestTrue(
			*FString::Printf(
				TEXT("At least 95 percent of %s frames stay inside one 60 FPS frame"),
				Profile.Name),
			P95MovementFrameMilliseconds < 16.67);
		TestTrue(
			*FString::Printf(
				TEXT("%s chunk-boundary frames stay below two 60 FPS frames"),
				Profile.Name),
			MaximumBoundaryFrameMilliseconds < 33.34);
		TestTrue(
			TEXT("Movement does not grow the resident simulation window"),
			WorldActor->GetResidentMaterialChunkCount()
				<= ExpectedActiveDiameter * ExpectedActiveDiameter);
		TestTrue(
			TEXT("Movement does not load the full terrain render grid"),
			WorldActor->GetVisibleTerrainTriangleCount() < 3000000);
		TestTrue(
			TEXT("Terrain mesh hot cache stays within its configured cap"),
			WorldActor->GetCachedTerrainChunkCount()
				<= WorldActor->GetTerrainChunkCacheLimit());
		TestTrue(
			*FString::Printf(
				TEXT("%s traversal does not accumulate streamed Source actors"),
				Profile.Name),
			WorldActor->GetGeneratedFragmentSourceCount() < 500);
	}

	PlayerCharacter->SetActorLocation(FVector::ZeroVector);
	WorldActor->Tick(0.0f);
	double MaximumStreamingFrameMilliseconds = 0.0;
	int32 StreamingFrames = 0;
	while (WorldActor->GetPendingFragmentSourceSpawnCount() > 0
		&& StreamingFrames < 360)
	{
		const double FrameStart = FPlatformTime::Seconds();
		WorldActor->Tick(1.0f / 60.0f);
		MaximumStreamingFrameMilliseconds = FMath::Max(
			MaximumStreamingFrameMilliseconds,
			(FPlatformTime::Seconds() - FrameStart) * 1000.0);
		++StreamingFrames;
	}
	AddInfo(FString::Printf(
		TEXT("LargeWorld decoration streaming: %d frames, max %.2f ms"),
		StreamingFrames,
		MaximumStreamingFrameMilliseconds));
	TestEqual(
		TEXT("Nearby decorations finish progressive streaming within six seconds"),
		WorldActor->GetPendingFragmentSourceSpawnCount(),
		0);
	TestTrue(
		TEXT("Budgeted decoration spawning stays inside a 60 FPS frame"),
		MaximumStreamingFrameMilliseconds < 16.67);
	const double ControlTickStart = FPlatformTime::Seconds();
	for (int32 Step = 0; Step < 120; ++Step)
	{
		WorldActor->Tick(0.1f);
	}
	const double ControlTickMilliseconds =
		(FPlatformTime::Seconds() - ControlTickStart) * 1000.0;
	AddInfo(FString::Printf(
		TEXT("LargeWorld 120 pre-ignition world ticks: %.2f ms"),
		ControlTickMilliseconds));
	TestTrue(
		TEXT("A nearby generated tree can be ignited under load"),
		WorldActor->IgniteFirstGeneratedTree(9091));
	double SourceActorTickSeconds = 0.0;
	double WorldActorTickSeconds = 0.0;
	const double CombustionStart = FPlatformTime::Seconds();
	for (int32 Step = 0; Step < 120; ++Step)
	{
		const double SourceActorTickStart = FPlatformTime::Seconds();
		for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
		{
			It->Tick(0.1f);
		}
		SourceActorTickSeconds +=
			FPlatformTime::Seconds() - SourceActorTickStart;
		const double WorldActorTickStart = FPlatformTime::Seconds();
		WorldActor->Tick(0.1f);
		WorldActorTickSeconds +=
			FPlatformTime::Seconds() - WorldActorTickStart;
	}
	const double CombustionMilliseconds =
		(FPlatformTime::Seconds() - CombustionStart) * 1000.0;
	AddInfo(FString::Printf(
		TEXT("LargeWorld 120 movement/material/combustion ticks: %.2f ms, ground replication=%d bytes"),
		CombustionMilliseconds,
		WorldActor->GetReplicatedGroundCombustionByteCount()));
	AddInfo(FString::Printf(
		TEXT("LargeWorld combustion breakdown: Source Actor ticks %.2f ms, World Actor ticks %.2f ms"),
		SourceActorTickSeconds * 1000.0,
		WorldActorTickSeconds * 1000.0));
	TestTrue(
		TEXT("Burning simulation remains inside the 60 FPS CPU budget"),
		CombustionMilliseconds < 2000.0);
	TestTrue(
		TEXT("Combustion adds less than 275 ms over the no-fire control ticks"),
		CombustionMilliseconds - ControlTickMilliseconds < 275.0);
	constexpr int32 GroundCellCount =
		MatterFlux::PlayableLevel::TerrainCellsX
		* MatterFlux::PlayableLevel::TerrainCellsY;
	const int32 RawGroundStateBytes =
		2 * (1 + FMath::DivideAndRoundUp(GroundCellCount, 8));
	TestTrue(
		TEXT("Localized ground fire compresses below two full bit masks"),
		WorldActor->GetReplicatedGroundCombustionByteCount()
			< RawGroundStateBytes);
	TestTrue(
		TEXT("Combustion still creates solid residue in the stress map"),
		WorldActor->GetCombustionResidueCellCount() > 0);
	return true;
}
