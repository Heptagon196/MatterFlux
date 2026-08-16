#include "Material/MatterFluxCombustion.h"
#include "Material/MatterFluxGroundCombustionRuntime.h"
#include "Material/MatterFluxSourceCombustionRuntime.h"
#include "Fragment/Fragment2DActor.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/Fragment2DSourceStreamingState.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Game/MatterFluxPlayableLevel.h"
#include "IMatterFluxScriptRuntime.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"

#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSourceCombustionRuntimeFixedStepTest,
	"MatterFlux.Combustion.SourceRuntimePreservesFixedStepDebtWithoutActor",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxSourceCombustionRuntimeFixedStepTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = 4;
	Mask.Height = 1;
	Mask.CellSize = 4.0f;
	Mask.SolidMask.Init(1, Mask.Width * Mask.Height);

	FMatterFluxCombustionDefinition Rule;
	Rule.Id = TEXT("source_runtime_fire");
	Rule.FuelMaterial = TEXT("wood");
	Rule.FlameMaterial = TEXT("fire");
	Rule.SmokeMaterial = TEXT("smoke");
	Rule.ResidueMaterial = TEXT("charcoal");
	Rule.IgnitionChancePermille = 1000;
	Rule.SpreadChancePermille = 1000;
	Rule.SmokeChancePermille = 1000;
	Rule.BurnDurationSteps = 2;

	MatterFlux::Combustion::FSourceRuntimeSettings Settings;
	MatterFlux::Combustion::FSourceCombustionRuntime Original;
	FString Error;
	TestTrue(
		TEXT("Source runtime initializes without a UObject"),
		Original.Initialize(Settings, Mask, Rule, 1337, Error));
	TestTrue(
		TEXT("Nearest fuel cell ignites deterministically"),
		Original.IgniteNearest(FIntPoint(-10, 0), TEXT("fire")));

	const MatterFlux::Combustion::FSourceAdvanceResult BeforeStep =
		Original.AdvanceAuthority(0.06f);
	TestEqual(TEXT("Sub-step time does not advance simulation"),
		BeforeStep.Steps,
		0);

	MatterFlux::Combustion::FSourceRuntimeSnapshot Snapshot;
	TestTrue(TEXT("Runtime captures fixed-step debt"),
		Original.CaptureState(Snapshot));
	TestTrue(TEXT("Snapshot retains the sub-step accumulator"),
		FMath::IsNearlyEqual(Snapshot.CombustionAccumulator, 0.06f));

	MatterFlux::Combustion::FSourceCombustionRuntime Restored;
	TestTrue(TEXT("Runtime restores transactionally"),
		Restored.RestoreState(Settings, Snapshot, Rule, Error));
	const MatterFlux::Combustion::FSourceAdvanceResult OriginalStep =
		Original.AdvanceAuthority(0.04f);
	const MatterFlux::Combustion::FSourceAdvanceResult RestoredStep =
		Restored.AdvanceAuthority(0.04f);

	TestEqual(TEXT("Debt completes exactly one fixed step"),
		OriginalStep.Steps,
		1);
	TestEqual(TEXT("Restored runtime completes the same step count"),
		RestoredStep.Steps,
		OriginalStep.Steps);
	TestTrue(TEXT("Smoke emissions are deterministic"),
		RestoredStep.SmokeEmissionCells
			== OriginalStep.SmokeEmissionCells);
	TestTrue(TEXT("Changed cells are deterministic"),
		RestoredStep.ChangedCellIndices
			== OriginalStep.ChangedCellIndices);
	TestTrue(TEXT("Fuel state stays identical"),
		Restored.GetFuelMask() == Original.GetFuelMask());
	TestTrue(TEXT("Residue state stays identical"),
		Restored.GetResidueMask() == Original.GetResidueMask());
	TestTrue(TEXT("Burning state stays identical"),
		Restored.GetBurningMask() == Original.GetBurningMask());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSourceCombustionSnapshotReuseTest,
	"MatterFlux.Combustion.SourceSnapshotCaptureReusesCallerStorageTransactionally",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxSourceCombustionSnapshotReuseTest::RunTest(
	const FString& Parameters)
{
	constexpr int32 Width = 64;
	constexpr int32 Height = 64;
	constexpr int32 CellCount = Width * Height;
	FFragmentSourceMask Mask;
	Mask.Width = Width;
	Mask.Height = Height;
	Mask.CellSize = 4.0f;
	Mask.SolidMask.Init(1, CellCount);

	FMatterFluxCombustionDefinition Rule;
	Rule.Id = TEXT("source_snapshot_reuse");
	Rule.FuelMaterial = TEXT("wood");
	Rule.FlameMaterial = TEXT("fire");
	Rule.SmokeMaterial = TEXT("smoke");
	Rule.ResidueMaterial = TEXT("charcoal");
	Rule.IgnitionChancePermille = 1000;
	Rule.SpreadChancePermille = 0;
	Rule.BurnDurationSteps = 8;

	MatterFlux::Combustion::FSourceCombustionRuntime Runtime;
	FString Error;
	if (!TestTrue(
		TEXT("Source runtime initializes for reusable capture"),
		Runtime.Initialize(
			MatterFlux::Combustion::FSourceRuntimeSettings(),
			Mask,
			Rule,
			4242,
			Error)))
	{
		AddError(Error);
		return false;
	}

	MatterFlux::Combustion::FSourceRuntimeSnapshot Snapshot;
	Snapshot.CombustionState.FuelMask.Reserve(CellCount * 2);
	Snapshot.CombustionState.ResidueMask.Reserve(CellCount * 2);
	Snapshot.CombustionState.BurningMask.Reserve(CellCount * 2);
	const SIZE_T ReservedBytes =
		Snapshot.CombustionState.FuelMask.GetAllocatedSize()
		+ Snapshot.CombustionState.ResidueMask.GetAllocatedSize()
		+ Snapshot.CombustionState.BurningMask.GetAllocatedSize();
	if (!TestTrue(
		TEXT("Valid capture succeeds"),
		Runtime.CaptureState(Snapshot)))
	{
		return false;
	}
	const SIZE_T CapturedBytes =
		Snapshot.CombustionState.FuelMask.GetAllocatedSize()
		+ Snapshot.CombustionState.ResidueMask.GetAllocatedSize()
		+ Snapshot.CombustionState.BurningMask.GetAllocatedSize();
	TestTrue(
		TEXT("Capture retains caller-owned mask capacity for the next fixed step"),
		CapturedBytes >= ReservedBytes);
	constexpr int32 CaptureCount = 4096;
	const double CaptureStartSeconds = FPlatformTime::Seconds();
	for (int32 CaptureIndex = 0;
		CaptureIndex < CaptureCount;
		++CaptureIndex)
	{
		if (!Runtime.CaptureState(Snapshot))
		{
			AddError(TEXT("Repeated reusable capture failed"));
			return false;
		}
	}
	const double CaptureMilliseconds =
		(FPlatformTime::Seconds() - CaptureStartSeconds) * 1000.0;
	AddInfo(FString::Printf(
		TEXT("Source snapshot: %d captures of three %d-cell masks in %.2f ms"),
		CaptureCount,
		CellCount,
		CaptureMilliseconds));
	TestTrue(
		TEXT("Reusable Source snapshots stay inside the fixed-step copy budget"),
		CaptureMilliseconds < 100.0);

	const MatterFlux::Combustion::FSourceRuntimeSnapshot Committed = Snapshot;
	MatterFlux::Combustion::FSourceCombustionRuntime InvalidRuntime;
	TestFalse(
		TEXT("An uninitialized runtime rejects capture"),
		InvalidRuntime.CaptureState(Snapshot));
	TestTrue(
		TEXT("Rejected capture preserves the committed combustion snapshot"),
		Snapshot.CombustionState.RuleId
			== Committed.CombustionState.RuleId
			&& Snapshot.CombustionState.Tick
				== Committed.CombustionState.Tick
			&& Snapshot.CombustionState.FuelMask
				== Committed.CombustionState.FuelMask
			&& Snapshot.CombustionState.ResidueMask
				== Committed.CombustionState.ResidueMask
			&& Snapshot.CombustionState.BurningMask
				== Committed.CombustionState.BurningMask
			&& Snapshot.CombustionAccumulator
				== Committed.CombustionAccumulator
			&& Snapshot.TotalSmokeEmissionCount
				== Committed.TotalSmokeEmissionCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSourceStreamingMaskStorageTest,
	"MatterFlux.Combustion.StreamingStateStoresOneCanonicalRuntimeMask",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxSourceStreamingMaskStorageTest::RunTest(
	const FString& Parameters)
{
	constexpr int32 Width = 8;
	constexpr int32 Height = 8;
	constexpr int32 CellCount = Width * Height;
	FFragmentSourceMask Mask;
	Mask.Width = Width;
	Mask.Height = Height;
	Mask.CellSize = 4.0f;
	Mask.SolidMask.Init(1, CellCount);

	FMatterFluxCombustionDefinition Rule;
	Rule.Id = TEXT("canonical_streaming_mask");
	Rule.FuelMaterial = TEXT("wood");
	Rule.FlameMaterial = TEXT("fire");
	Rule.SmokeMaterial = TEXT("smoke");
	Rule.ResidueMaterial = TEXT("charcoal");
	Rule.IgnitionChancePermille = 1000;
	Rule.SpreadChancePermille = 0;
	Rule.BurnDurationSteps = 8;

	MatterFlux::Combustion::FSourceCombustionRuntime Runtime;
	FString Error;
	if (!TestTrue(
		TEXT("Runtime initializes for canonical streaming capture"),
		Runtime.Initialize(
			MatterFlux::Combustion::FSourceRuntimeSettings(),
			Mask,
			Rule,
			8675309,
			Error)))
	{
		AddError(Error);
		return false;
	}

	FFragment2DSourceStreamingState State;
	if (!TestTrue(
		TEXT("Streaming state captures the combustion runtime"),
		State.CaptureCombustionState(Runtime)))
	{
		return false;
	}
	TestTrue(
		TEXT("Effective runtime mask is the captured fuel truth"),
		State.GetRuntimeMask() == Runtime.GetFuelMask());
	TestEqual(
		TEXT("Combusting state stores fuel, residue and burning exactly once"),
		State.GetStoredMaskValueCount(),
		CellCount * 3);

	const FFragment2DSourceStreamingState Committed = State;
	MatterFlux::Combustion::FSourceCombustionRuntime InvalidRuntime;
	TestFalse(
		TEXT("Invalid runtime capture is rejected"),
		State.CaptureCombustionState(InvalidRuntime));
	TestTrue(
		TEXT("Rejected capture preserves canonical mask truth"),
		State.GetRuntimeMask() == Committed.GetRuntimeMask()
			&& State.GetStoredMaskValueCount()
				== Committed.GetStoredMaskValueCount());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaskCombustionConsumesFuelTest,
	"MatterFlux.Combustion.FireSpreadsAndProducesSmokeAndSolidResidue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxMaskCombustionConsumesFuelTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = 3;
	Mask.Height = 1;
	Mask.CellSize = 10.0f;
	Mask.MinFragmentAreaPixels = 1;
	Mask.MaxFragmentsPerBreak = 4;
	Mask.SolidMask.Init(1, 3);

	FMatterFluxCombustionDefinition Rule;
	Rule.Id = TEXT("test_burn");
	Rule.FuelMaterial = TEXT("wood");
	Rule.FlameMaterial = TEXT("fire");
	Rule.SmokeMaterial = TEXT("smoke");
	Rule.ResidueMaterial = TEXT("charcoal");
	Rule.IgnitionChancePermille = 1000;
	Rule.SpreadChancePermille = 1000;
	Rule.BurnDurationSteps = 2;
	Rule.SmokeChancePermille = 1000;

	MatterFlux::Combustion::FMaskCombustion Simulation;
	TestTrue(
		TEXT("Valid fuel mask initializes"),
		Simulation.Initialize(Mask, Rule, 1337));
	TestTrue(
		TEXT("A flame ignites the selected fuel cell"),
		Simulation.Ignite(FIntPoint(1, 0), TEXT("fire")));

	int32 TotalSmoke = 0;
	for (int32 Step = 0; Step < 5; ++Step)
	{
		TotalSmoke += Simulation.Step().SmokeEmissionCells.Num();
	}

	TestFalse(TEXT("Fire finishes after consuming connected fuel"),
		Simulation.IsBurning());
	TestEqual(TEXT("Connected fuel is consumed"),
		Simulation.CountFuelCells(),
		0);
	TestEqual(TEXT("Every consumed cell leaves solid residue"),
		Simulation.CountResidueCells(),
		3);
	TestTrue(TEXT("Burning emits smoke particles"), TotalSmoke > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxCombustionDamageConstraintTest,
	"MatterFlux.Combustion.DamageCannotResurrectRemovedFuel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxCombustionDamageConstraintTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = 3;
	Mask.Height = 1;
	Mask.CellSize = 10.0f;
	Mask.MinFragmentAreaPixels = 1;
	Mask.MaxFragmentsPerBreak = 4;
	Mask.SolidMask.Init(1, 3);

	FMatterFluxCombustionDefinition Rule;
	Rule.Id = TEXT("damage_constraint");
	Rule.FuelMaterial = TEXT("wood");
	Rule.FlameMaterial = TEXT("fire");
	Rule.SmokeMaterial = TEXT("smoke");
	Rule.ResidueMaterial = TEXT("charcoal");
	Rule.IgnitionChancePermille = 1000;
	Rule.SpreadChancePermille = 1000;
	Rule.BurnDurationSteps = 3;
	Rule.SmokeChancePermille = 0;

	MatterFlux::Combustion::FMaskCombustion Simulation;
	TestTrue(TEXT("Fuel initializes"),
		Simulation.Initialize(Mask, Rule, 91));
	TestTrue(TEXT("Center fuel ignites"),
		Simulation.Ignite(FIntPoint(1, 0), TEXT("fire")));

	TArray<uint8> DamageMask{0, 1, 1};
	TestTrue(TEXT("Committed damage constrains combustion fuel"),
		Simulation.ConstrainFuelMask(DamageMask));
	for (int32 Step = 0; Step < 8; ++Step)
	{
		Simulation.Step();
	}
	TestEqual(TEXT("Removed fuel never returns"),
		Simulation.GetFuelMask()[0],
		static_cast<uint8>(0));
	TestEqual(TEXT("Removed fuel does not become combustion residue"),
		Simulation.GetResidueMask()[0],
		static_cast<uint8>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSourceCombustionCutSynchronizationTest,
	"MatterFlux.Combustion.CutImmediatelySynchronizesVisibleAndReplicatedMasks",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxSourceCombustionCutSynchronizationTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DSourceActor* Source = World
		? World->SpawnActor<AFragment2DSourceActor>()
		: nullptr;
	if (!TestNotNull(TEXT("Combustible source spawns"), Source))
	{
		return false;
	}
	Source->bDestroySourceOnFirstBreak = false;

	FFragmentSourceMask Mask;
	Mask.Width = 1;
	Mask.Height = 1;
	Mask.CellSize = 10.0f;
	Mask.MinFragmentAreaPixels = 1;
	Mask.MaxFragmentsPerBreak = 1;
	Mask.SupportMode = EFragmentSupportMode::Bottom;
	Mask.SolidMask = {1};
	if (!TestTrue(
		TEXT("Single-cell wood source initializes"),
		Source->InitializeFromProceduralMask(
			Mask,
			FGuid::NewDeterministicGuid(
				TEXT("CombustionCutSynchronization"),
				1),
			FLinearColor::White,
			TEXT("wood"))))
	{
		return false;
	}
	TestFalse(
		TEXT("Non-finite ignition location is rejected"),
		Source->IgniteAtWorldLocation(
			FVector(
				std::numeric_limits<double>::quiet_NaN(),
				0.0,
				0.0),
			TEXT("fire"),
			700));
	if (!TestTrue(
		TEXT("Finite ignition starts the source fire"),
		Source->IgniteAtWorldLocation(
			Source->GetActorLocation(),
			TEXT("fire"),
			701)))
	{
		return false;
	}
	TestEqual(
		TEXT("One visible cell is burning before the cut"),
		Source->GetBurningCellCount(),
		1);

	FFragmentDamageEvent Event;
	Event.SourceId = Source->SourceId;
	Event.BaseRevision = Source->Revision;
	Event.DamageShape.Type = EFragmentDamageShapeType::Circle;
	Event.DamageShape.WorldTransform = Source->GetActorTransform();
	Event.DamageShape.Radius = 10.0f;
	Event.DamagePower = 0.0f;
	Event.EventSeed = 702;
	UFragmentSimulationSubsystem* Subsystem =
		World->GetSubsystem<UFragmentSimulationSubsystem>();
	if (!TestNotNull(TEXT("Fragment subsystem exists"), Subsystem)
		|| !TestTrue(
			TEXT("Cutting the burning cell commits"),
			Subsystem->RequestFragmentDamage(Source, Event)))
	{
		return false;
	}

	TestEqual(
		TEXT("Cut clears visible burning state immediately"),
		Source->GetBurningCellCount(),
		0);
	TestEqual(
		TEXT("Cut removes the combustion fuel"),
		Source->GetRemainingFuelCellCount(),
		0);
	TestTrue(
		TEXT("Replicated combustion masks remain compact and synchronized"),
		Source->GetReplicatedCombustionByteCount() <= 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLargeCombustionMaskTest,
	"MatterFlux.Combustion.LargeSimulationMaskIsIndependentFromFragmentReplicationLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLargeCombustionMaskTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = MatterFlux::PlayableLevel::TerrainCellsX;
	Mask.Height = MatterFlux::PlayableLevel::TerrainCellsY;
	Mask.SolidMask.Init(1, Mask.Width * Mask.Height);

	FMatterFluxCombustionDefinition Rule;
	Rule.Id = TEXT("large_ground_burn");
	Rule.FuelMaterial = TEXT("grassland");
	Rule.FlameMaterial = TEXT("fire");
	Rule.SmokeMaterial = TEXT("smoke");
	Rule.ResidueMaterial = TEXT("ash");
	Rule.IgnitionChancePermille = 1000;
	Rule.SpreadChancePermille = 25;
	Rule.BurnDurationSteps = 8;
	Rule.SmokeChancePermille = 420;

	MatterFlux::Combustion::FMaskCombustion Simulation;
	TestTrue(
		TEXT("Large local simulation masks are not constrained by fragment replication dimensions"),
		Simulation.Initialize(Mask, Rule, 1337));
	TestTrue(
		TEXT("The initialized large mask can ignite"),
		Simulation.Ignite(
			FIntPoint(Mask.Width / 2, Mask.Height / 2),
			TEXT("fire")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxGroundCombustionRuntimeChunkBatchTest,
	"MatterFlux.Combustion.GroundRuntimeBatchesOnlyChangedChunks",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxGroundCombustionRuntimeChunkBatchTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = 128;
	Mask.Height = 64;
	Mask.CellSize = 10.0f;
	Mask.SolidMask.Init(1, Mask.Width * Mask.Height);

	FMatterFluxCombustionDefinition Rule;
	Rule.Id = TEXT("ground_runtime_chunk_batch");
	Rule.FuelMaterial = TEXT("grassland");
	Rule.FlameMaterial = TEXT("fire");
	Rule.SmokeMaterial = TEXT("smoke");
	Rule.ResidueMaterial = TEXT("ash");
	Rule.IgnitionChancePermille = 1000;
	Rule.SpreadChancePermille = 0;
	Rule.BurnDurationSteps = 2;
	Rule.SmokeChancePermille = 0;

	MatterFlux::Combustion::FGroundRuntimeSettings Settings;
	Settings.Width = Mask.Width;
	Settings.Height = Mask.Height;
	MatterFlux::Combustion::FGroundCombustionRuntime Runtime;
	FString Error;
	if (!TestTrue(
		TEXT("Ground runtime initializes through one deep interface"),
		Runtime.Initialize(Settings, Mask, Rule, 7001, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(
		TEXT("Ignition marks the containing replication chunk"),
		Runtime.Ignite(FIntPoint(65, 10), Rule.FlameMaterial));
	TArray<int32> BurningCellIndices;
	Runtime.GatherBurningCellIndices(BurningCellIndices);
	TestEqual(
		TEXT("The runtime exposes one sparse burning cell after ignition"),
		BurningCellIndices.Num(),
		1);
	if (BurningCellIndices.Num() == 1)
	{
		TestEqual(
			TEXT("The sparse cell index matches the ignited coordinate"),
			BurningCellIndices[0],
			10 * Mask.Width + 65);
	}
	TArray<FIntPoint> BurningChunks;
	Runtime.GatherBurningChunkCoordinates(BurningChunks);
	TestTrue(
		TEXT("Sparse burning cells collapse to one deterministic active chunk"),
		BurningChunks == TArray<FIntPoint>({ FIntPoint(1, 0) }));
	TestTrue(
		TEXT("A second burning cell in the same chunk is accepted"),
		Runtime.Ignite(FIntPoint(66, 10), Rule.FlameMaterial));
	Runtime.GatherBurningChunkCoordinates(BurningChunks);
	TestTrue(
		TEXT("Multiple burning cells in one chunk still produce one query region"),
		BurningChunks == TArray<FIntPoint>({ FIntPoint(1, 0) }));
	const TArray<FIntPoint> VisibleChunks = { FIntPoint(1, 0) };
	TArray<int32> VisibleResidueCells;
	TArray<int32> VisibleBurningCells;
	Runtime.GatherVisibleCellIndicesForChunks(
		VisibleChunks,
		VisibleResidueCells,
		VisibleBurningCells);
	TestTrue(
		TEXT("Visible chunk lookup returns only its current burning cells"),
		VisibleResidueCells.IsEmpty()
			&& VisibleBurningCells == TArray<int32>(
				{ 10 * Mask.Width + 65, 10 * Mask.Width + 66 }));

	TArray<FMatterFluxGroundStateChunk> Batch;
	if (!TestTrue(
		TEXT("Pending replication is built transactionally"),
		Runtime.BuildPendingReplication(Batch, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Only one changed chunk is published"), Batch.Num(), 1);
	if (Batch.Num() == 1)
	{
		TestEqual(
			TEXT("The second horizontal chunk is selected"),
			Batch[0].ChunkCoordinate,
			FIntPoint(1, 0));
		TestEqual(TEXT("The batch advances revision once"), Batch[0].Revision, 1);
	}
	TestFalse(
		TEXT("Committed replication leaves no pending chunks"),
		Runtime.HasPendingReplication());
	Runtime.AdvanceAuthority(Settings.StepSeconds);
	Runtime.AdvanceAuthority(Settings.StepSeconds);
	Runtime.GatherBurningCellIndices(BurningCellIndices);
	Runtime.GatherBurningChunkCoordinates(BurningChunks);
	TestEqual(
		TEXT("Sparse burning cells are removed after fixed-step fuel exhaustion"),
		BurningCellIndices.Num(),
		0);
	TestTrue(
		TEXT("Extinguished ground leaves no active combustion chunks"),
		BurningChunks.IsEmpty());
	TArray<int32> ResidueCellIndices;
	Runtime.GatherResidueCellIndices(ResidueCellIndices);
	TestTrue(
		TEXT("Burned-out cells are exposed as a stable sparse residue set"),
		ResidueCellIndices == TArray<int32>(
			{ 10 * Mask.Width + 65, 10 * Mask.Width + 66 }));
	Runtime.GatherVisibleCellIndicesForChunks(
		VisibleChunks,
		VisibleResidueCells,
		VisibleBurningCells);
	TestTrue(
		TEXT("Visible chunk lookup replaces exhausted fire with residue"),
		VisibleBurningCells.IsEmpty()
			&& VisibleResidueCells == ResidueCellIndices);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxGroundCombustionRuntimeReplicationGuardTest,
	"MatterFlux.Combustion.GroundRuntimeRejectsCorruptionAtomically",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxGroundCombustionRuntimeReplicationGuardTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = 64;
	Mask.Height = 64;
	Mask.CellSize = 10.0f;
	Mask.SolidMask.Init(1, Mask.Width * Mask.Height);

	FMatterFluxCombustionDefinition Rule;
	Rule.Id = TEXT("ground_runtime_replication_guard");
	Rule.FuelMaterial = TEXT("grassland");
	Rule.FlameMaterial = TEXT("fire");
	Rule.SmokeMaterial = TEXT("smoke");
	Rule.ResidueMaterial = TEXT("ash");
	Rule.IgnitionChancePermille = 1000;
	Rule.SpreadChancePermille = 0;
	Rule.BurnDurationSteps = 2;

	MatterFlux::Combustion::FGroundRuntimeSettings Settings;
	Settings.Width = Mask.Width;
	Settings.Height = Mask.Height;
	MatterFlux::Combustion::FGroundCombustionRuntime Authority;
	MatterFlux::Combustion::FGroundCombustionRuntime Client;
	FString Error;
	if (!TestTrue(TEXT("Authority runtime initializes"),
		Authority.Initialize(Settings, Mask, Rule, 71, Error))
		|| !TestTrue(TEXT("Client runtime initializes"),
			Client.Initialize(Settings, Mask, Rule, 71, Error)))
	{
		AddError(Error);
		return false;
	}
	Authority.Ignite(FIntPoint(2, 3), Rule.FlameMaterial);
	Authority.AdvanceAuthority(Settings.StepSeconds);
	TArray<FMatterFluxGroundStateChunk> Batch;
	if (!TestTrue(TEXT("Authority publishes the changed state"),
		Authority.BuildPendingReplication(Batch, Error))
		|| Batch.Num() != 1)
	{
		AddError(Error);
		return false;
	}

	FMatterFluxGroundStateChunk Corrupt = Batch[0];
	Corrupt.StateHash ^= 0x1u;
	const TArray<uint8> BeforeResidue = Client.GetResidueMask();
	const TArray<uint8> BeforeBurning = Client.GetBurningMask();
	TestEqual(
		TEXT("A corrupt chunk is rejected"),
		Client.ApplyReplicatedChunk(Corrupt, Error),
		MatterFlux::Combustion::EGroundChunkApplyResult::Rejected);
	TestTrue(
		TEXT("Rejected residue is not partially applied"),
		Client.GetResidueMask() == BeforeResidue);
	TestTrue(
		TEXT("Rejected burning state is not partially applied"),
		Client.GetBurningMask() == BeforeBurning);
	TestEqual(
		TEXT("The valid payload applies after a corrupt payload at the same revision"),
		Client.ApplyReplicatedChunk(Batch[0], Error),
		MatterFlux::Combustion::EGroundChunkApplyResult::Applied);
	TArray<int32> AuthorityBurningCells;
	TArray<int32> ClientBurningCells;
	Authority.GatherBurningCellIndices(AuthorityBurningCells);
	Client.GatherBurningCellIndices(ClientBurningCells);
	TestTrue(
		TEXT("Replicated sparse burning cells match authority"),
		ClientBurningCells == AuthorityBurningCells);
	TestEqual(
		TEXT("A repeated payload is idempotent"),
		Client.ApplyReplicatedChunk(Batch[0], Error),
		MatterFlux::Combustion::EGroundChunkApplyResult::NoChange);

	MatterFlux::Combustion::FGroundRuntimeSnapshot WrapSnapshot;
	if (!TestTrue(TEXT("Authority state captures for wrap testing"),
		Authority.CaptureState(WrapSnapshot)))
	{
		return false;
	}
	WrapSnapshot.Revision = MAX_int32;
	MatterFlux::Combustion::FGroundCombustionRuntime WrappedAuthority;
	MatterFlux::Combustion::FGroundCombustionRuntime WrappedClient;
	if (!TestTrue(TEXT("Wrapped authority restores"),
		WrappedAuthority.RestoreState(
			Settings, WrapSnapshot, Rule, Error))
		|| !TestTrue(TEXT("Wrapped client initializes"),
			WrappedClient.Initialize(Settings, Mask, Rule, 71, Error)))
	{
		AddError(Error);
		return false;
	}
	TArray<FMatterFluxGroundStateChunk> WrappedBatch;
	WrappedAuthority.BuildInitialReplication(WrappedBatch, Error);
	if (!TestEqual(TEXT("Wrapped initial batch has one chunk"),
		WrappedBatch.Num(), 1))
	{
		return false;
	}
	TestEqual(
		TEXT("Client accepts the maximum revision"),
		WrappedClient.ApplyReplicatedChunk(WrappedBatch[0], Error),
		MatterFlux::Combustion::EGroundChunkApplyResult::Applied);
	WrappedAuthority.Ignite(FIntPoint(5, 5), Rule.FlameMaterial);
	WrappedAuthority.BuildPendingReplication(WrappedBatch, Error);
	TestEqual(
		TEXT("Revision wraps from MAX_int32 to zero"),
		WrappedBatch[0].Revision,
		0);
	TestEqual(
		TEXT("Client accepts the wrapped revision as newer"),
		WrappedClient.ApplyReplicatedChunk(WrappedBatch[0], Error),
		MatterFlux::Combustion::EGroundChunkApplyResult::Applied);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxGroundCombustionRuntimeSnapshotDebtTest,
	"MatterFlux.Combustion.GroundRuntimeSnapshotPreservesFixedStepDebt",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxGroundCombustionRuntimeSnapshotDebtTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = 64;
	Mask.Height = 64;
	Mask.CellSize = 10.0f;
	Mask.SolidMask.Init(1, Mask.Width * Mask.Height);

	FMatterFluxCombustionDefinition Rule;
	Rule.Id = TEXT("ground_runtime_snapshot_debt");
	Rule.FuelMaterial = TEXT("grassland");
	Rule.FlameMaterial = TEXT("fire");
	Rule.SmokeMaterial = TEXT("smoke");
	Rule.ResidueMaterial = TEXT("ash");
	Rule.IgnitionChancePermille = 1000;
	Rule.SpreadChancePermille = 0;
	Rule.BurnDurationSteps = 1;

	MatterFlux::Combustion::FGroundRuntimeSettings Settings;
	Settings.Width = Mask.Width;
	Settings.Height = Mask.Height;
	MatterFlux::Combustion::FGroundCombustionRuntime Original;
	FString Error;
	if (!TestTrue(TEXT("Original runtime initializes"),
		Original.Initialize(Settings, Mask, Rule, 91, Error)))
	{
		AddError(Error);
		return false;
	}
	Original.Ignite(FIntPoint(4, 5), Rule.FlameMaterial);
	TestEqual(
		TEXT("Half a fixed step performs no simulation step"),
		Original.AdvanceAuthority(Settings.StepSeconds * 0.5f).Steps,
		0);
	MatterFlux::Combustion::FGroundRuntimeSnapshot Snapshot;
	if (!TestTrue(TEXT("Runtime snapshot captures scheduler debt"),
		Original.CaptureState(Snapshot)))
	{
		return false;
	}
	MatterFlux::Combustion::FGroundCombustionRuntime Restored;
	if (!TestTrue(TEXT("Runtime snapshot restores atomically"),
		Restored.RestoreState(Settings, Snapshot, Rule, Error)))
	{
		AddError(Error);
		return false;
	}
	TArray<int32> OriginalBurningCells;
	TArray<int32> RestoredBurningCells;
	Original.GatherBurningCellIndices(OriginalBurningCells);
	Restored.GatherBurningCellIndices(RestoredBurningCells);
	TestTrue(
		TEXT("Restored sparse burning cells match the captured runtime"),
		RestoredBurningCells == OriginalBurningCells
			&& !RestoredBurningCells.IsEmpty());
	TestEqual(
		TEXT("The restored half-step completes exactly one simulation step"),
		Restored.AdvanceAuthority(Settings.StepSeconds * 0.5f).Steps,
		1);
	Original.AdvanceAuthority(Settings.StepSeconds * 0.5f);
	TestTrue(
		TEXT("Restored state matches uninterrupted execution"),
		Restored.GetResidueMask() == Original.GetResidueMask());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSourceCombustionReplicationBudgetTest,
	"MatterFlux.Combustion.SourceReplicationBitPacksAllMasks",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxSourceCombustionReplicationBudgetTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DSourceActor* Source = World
		? World->SpawnActor<AFragment2DSourceActor>()
		: nullptr;
	if (!TestNotNull(TEXT("Combustible source spawns"), Source))
	{
		return false;
	}

	FFragmentSourceMask Mask;
	Mask.Width = 256;
	Mask.Height = 256;
	Mask.CellSize = 4.0f;
	Mask.MinFragmentAreaPixels = 1;
	Mask.MaxFragmentsPerBreak = 16;
	Mask.SolidMask.Init(1, Mask.Width * Mask.Height);
	if (!TestTrue(TEXT("Maximum source mask initializes"),
		Source->InitializeFromProceduralMask(
			Mask,
			FGuid::NewDeterministicGuid(
				TEXT("CombustionReplicationBudget"),
				1),
			FLinearColor::White,
			TEXT("wood"))))
	{
		return false;
	}
	if (!TestTrue(TEXT("Maximum source can ignite"),
		Source->IgniteAtWorldLocation(
			Source->GetActorLocation(),
			TEXT("fire"),
			191)))
	{
		return false;
	}

	const int32 MaximumPackedBytes =
		3 * FMath::DivideAndRoundUp(
			Mask.Width * Mask.Height,
			8);
	TestTrue(TEXT("Three replicated masks are bit packed"),
		Source->GetReplicatedCombustionByteCount()
			<= MaximumPackedBytes);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPlayableTreeCombustionTest,
	"MatterFlux.Combustion.PlayableTreeBurnsFromTrunkIntoCanopy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxPlayableTreeCombustionTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor =
		World
			? World->SpawnActor<AMatterFluxPlayableWorldActor>()
			: nullptr;
	if (!TestNotNull(TEXT("Playable world spawns"), WorldActor))
	{
		return false;
	}
	WorldActor->Regenerate(1337);
	TestNotNull(TEXT("External tree ignition command is registered"),
		IConsoleManager::Get().FindConsoleObject(
			TEXT("mf.Combustion.IgniteTree")));
	TestTrue(TEXT("A generated tree trunk can be ignited"),
		WorldActor->IgniteFirstGeneratedTree(991));
	int32 MaterializedSourcesAfterIgnition = 0;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		MaterializedSourcesAfterIgnition +=
			!It->IsActorBeingDestroyed() ? 1 : 0;
	}
	TestEqual(
		TEXT("Static combustion remains logical instead of allocating Source Actors"),
		MaterializedSourcesAfterIgnition,
		0);

	for (int32 Step = 0; Step < 100; ++Step)
	{
		WorldActor->Tick(0.1f);
	}

	const int32 BurnedWoodCells =
		WorldActor->GetLogicalCombustionResidueCellCount(TEXT("wood"));
	const int32 BurnedLeafCells =
		WorldActor->GetLogicalCombustionResidueCellCount(TEXT("leaf"));
	const int32 SmokeEmissions =
		WorldActor->GetLogicalCombustionSmokeEmissionCount();
	TestTrue(TEXT("The trunk becomes charcoal"), BurnedWoodCells > 0);
	TestTrue(TEXT("Fire crosses source boundaries into the canopy"),
		BurnedLeafCells > 0);
	TestTrue(TEXT("The burning tree generates smoke particles"),
		SmokeEmissions > 0);
	TestTrue(TEXT("Fire scorches the grassland beneath the tree"),
		WorldActor->GetScorchedGroundCellCount() > 0);
	TestTrue(TEXT("A single tree fire does not instantly consume the map"),
		WorldActor->GetScorchedGroundCellCount()
			< MatterFlux::PlayableLevel::TerrainCellsX
				* MatterFlux::PlayableLevel::TerrainCellsY / 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterializedFireKeepsLogicalNeighborsTest,
	"MatterFlux.Combustion.MaterializedFireKeepsLogicalNeighborsUnmaterialized",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxMaterializedFireKeepsLogicalNeighborsTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor =
		World ? World->SpawnActor<AMatterFluxPlayableWorldActor>() : nullptr;
	if (!TestNotNull(TEXT("Playable world spawns"), WorldActor))
	{
		return false;
	}

	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	MatterFlux::PlayableLevel::FLevelLayout Layout;
	if (!TestTrue(
		TEXT("Reference forest layout builds"),
		MatterFlux::PlayableLevel::BuildLevelLayout(
			1337,
			Layout,
			Registry.Get())))
	{
		return false;
	}
	WorldActor->Regenerate(1337);

	const MatterFlux::PlayableLevel::FLevelFragmentSource* Trunk =
		Layout.FragmentSources.FindByPredicate(
			[](const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
			{
				return Source.Name == TEXT("TreeTrunk")
					&& Source.bAggregateRoot;
			});
	const MatterFlux::PlayableLevel::FLevelFragmentSource* Leaf =
		Trunk
			? Layout.FragmentSources.FindByPredicate(
				[Trunk](const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
				{
					return Source.AggregateId == Trunk->AggregateId
						&& Source.MaterialId == TEXT("leaf");
				})
			: nullptr;
	if (!TestNotNull(TEXT("Generated tree trunk exists"), Trunk)
		|| !TestNotNull(TEXT("Generated tree leaf layer exists"), Leaf))
	{
		return false;
	}

	const auto BuildWorldBounds =
		[WorldActor](
			const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
		{
			const FVector HalfExtent(
				Source.Mask.Width * Source.Mask.CellSize * 0.5f,
				Source.Mask.CellSize,
				Source.Mask.Height * Source.Mask.CellSize * 0.5f);
			return FBox(-HalfExtent, HalfExtent).TransformBy(
				(Source.Transform * WorldActor->GetActorTransform())
					.ToMatrixWithScale());
		};
	const FBox TrunkBounds = BuildWorldBounds(*Trunk);
	const FBox LeafBounds = BuildWorldBounds(*Leaf);
	TArray<AFragment2DSourceActor*> Materialized;
	WorldActor->GatherFragmentSourcesInBounds(TrunkBounds, Materialized);
	AFragment2DSourceActor* TrunkActor = nullptr;
	for (AFragment2DSourceActor* Source : Materialized)
	{
		if (Source && Source->SourceId == Trunk->SourceId)
		{
			TrunkActor = Source;
		}
	}
	if (!TestNotNull(TEXT("Tree trunk materializes"), TrunkActor))
	{
		return false;
	}
	for (AFragment2DSourceActor* Source : Materialized)
	{
		if (Source && Source != TrunkActor)
		{
			WorldActor->DematerializeFragmentSource(Source->SourceId);
		}
	}
	const int32 MaterializedBeforePropagation =
		WorldActor->GetGeneratedFragmentSourceCount();
	if (!TestEqual(
		TEXT("Only the selected trunk remains materialized"),
		MaterializedBeforePropagation,
		1))
	{
		return false;
	}

	const FVector IgnitionPoint =
		TrunkBounds.GetClosestPointTo(LeafBounds.GetCenter());
	if (!TestTrue(
		TEXT("Materialized trunk ignites beside its logical canopy"),
		TrunkActor->IgniteAtWorldLocation(
			IgnitionPoint,
			TEXT("fire"),
			923)))
	{
		return false;
	}
	const int32 CombustingBeforePropagation =
		WorldActor->GetCombustingSourceCount();
	WorldActor->Tick(0.21f);

	TestTrue(
		TEXT("Fire propagates from the Actor into a logical source"),
		WorldActor->GetCombustingSourceCount()
			> CombustingBeforePropagation);
	TestEqual(
		TEXT("Propagation does not materialize logical neighbors"),
		WorldActor->GetGeneratedFragmentSourceCount(),
		MaterializedBeforePropagation);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterializedSourceCombustionHandoffTest,
	"MatterFlux.Combustion.MaterializedStaticSourceReturnsToLogicalRuntime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxMaterializedSourceCombustionHandoffTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor =
		World
			? World->SpawnActor<AMatterFluxPlayableWorldActor>()
			: nullptr;
	if (!TestNotNull(TEXT("Playable world spawns"), WorldActor))
	{
		return false;
	}
	WorldActor->Regenerate(1337);

	const int32 MaterializedCount =
		WorldActor->MaterializeFragmentSourcesForFlame(
			FVector(-100000.0f, 0.0f, 0.0f),
			FVector::ForwardVector,
			200000.0f,
			100000.0f,
			100000.0f,
			TEXT("fire"));
	if (!TestTrue(TEXT("Combustible sources can be materialized for interaction"),
		MaterializedCount > 0))
	{
		return false;
	}

	AFragment2DSourceActor* WoodSource = nullptr;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		if (!It->IsActorBeingDestroyed()
			&& It->SourceMaterialId == TEXT("wood")
			&& !It->bDetachedFromTerrain)
		{
			WoodSource = *It;
			break;
		}
	}
	if (!TestNotNull(TEXT("A materialized static trunk exists"), WoodSource))
	{
		return false;
	}

	const FGuid SourceId = WoodSource->SourceId;
	TestTrue(TEXT("The materialized trunk ignites"),
		WoodSource->IgniteAtWorldLocation(
			WoodSource->GetActorLocation(),
			TEXT("fire"),
			731));
	TestTrue(TEXT("The static trunk returns to the logical store"),
		WorldActor->DematerializeFragmentSource(SourceId));
	TestEqual(TEXT("Combustion remains active after Actor handoff"),
		WorldActor->GetCombustingSourceCount(),
		1);

	for (int32 Step = 0; Step < 30; ++Step)
	{
		WorldActor->Tick(0.1f);
	}

	AFragment2DSourceActor* ReturnedActor = nullptr;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		if (!It->IsActorBeingDestroyed() && It->SourceId == SourceId)
		{
			ReturnedActor = *It;
			break;
		}
	}
	TestNull(TEXT("The returned source no longer owns an Actor"), ReturnedActor);
	TestTrue(TEXT("Logical combustion continues producing residue"),
		WorldActor->GetLogicalCombustionResidueCellCount(TEXT("wood")) > 0);
	TestTrue(TEXT("Logical combustion continues emitting smoke"),
		WorldActor->GetLogicalCombustionSmokeEmissionCount() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxBurningAggregateMemberHandoffTest,
	"MatterFlux.Combustion.BurningTreeMemberMovesIntoOneDynamicCarrier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxBurningAggregateMemberHandoffTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor =
		World ? World->SpawnActor<AMatterFluxPlayableWorldActor>() : nullptr;
	if (!TestNotNull(TEXT("Playable world spawns"), WorldActor))
	{
		return false;
	}

	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	MatterFlux::PlayableLevel::FLevelLayout Layout;
	if (!TestTrue(
		TEXT("Reference forest layout builds"),
		MatterFlux::PlayableLevel::BuildLevelLayout(
			1337,
			Layout,
			Registry.Get())))
	{
		return false;
	}
	WorldActor->Regenerate(1337);

	const MatterFlux::PlayableLevel::FLevelFragmentSource* Trunk =
		Layout.FragmentSources.FindByPredicate(
			[](const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
			{
				return Source.Name == TEXT("TreeTrunk")
					&& Source.bAggregateRoot;
			});
	const MatterFlux::PlayableLevel::FLevelFragmentSource* Leaf =
		Trunk
			? Layout.FragmentSources.FindByPredicate(
				[Trunk](const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
				{
					return Source.AggregateId == Trunk->AggregateId
						&& !Source.bAggregateRoot
						&& Source.MaterialId == TEXT("leaf");
				})
			: nullptr;
	if (!TestNotNull(TEXT("Generated tree trunk exists"), Trunk)
		|| !TestNotNull(TEXT("Generated tree leaf layer exists"), Leaf))
	{
		return false;
	}

	const auto BuildWorldBounds =
		[WorldActor](
			const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
		{
			const FVector HalfExtent(
				Source.Mask.Width * Source.Mask.CellSize * 0.5f,
				Source.Mask.CellSize,
				Source.Mask.Height * Source.Mask.CellSize * 0.5f);
			return FBox(-HalfExtent, HalfExtent).TransformBy(
				(Source.Transform * WorldActor->GetActorTransform())
					.ToMatrixWithScale());
		};
	const FBox LeafBounds = BuildWorldBounds(*Leaf);
	if (!TestTrue(
		TEXT("A logical leaf layer ignites before the tree is felled"),
		WorldActor->IgniteLogicalFragmentSourcesInBounds(
			LeafBounds,
			LeafBounds.GetCenter(),
			TEXT("fire"),
			912) > 0))
	{
		return false;
	}

	TArray<AFragment2DSourceActor*> Materialized;
	WorldActor->GatherFragmentSourcesInBounds(
		BuildWorldBounds(*Trunk),
		Materialized);
	AFragment2DSourceActor* TrunkActor = nullptr;
	if (AFragment2DSourceActor** Found = Materialized.FindByPredicate(
		[Trunk](const AFragment2DSourceActor* Source)
		{
			return Source && Source->SourceId == Trunk->SourceId;
		}))
	{
		TrunkActor = *Found;
	}
	if (!TestNotNull(TEXT("Tree trunk materializes for cutting"), TrunkActor))
	{
		return false;
	}

	FFragmentDamageEvent Event;
	Event.SourceId = TrunkActor->SourceId;
	Event.BaseRevision = TrunkActor->Revision;
	Event.EventSeed = 913;
	Event.DamagePower = 400.0f;
	Event.DamageShape.Type = EFragmentDamageShapeType::Line;
	Event.DamageShape.WorldTransform = TrunkActor->GetActorTransform();
	Event.DamageShape.Extents.X = TrunkActor->GetCellSize()
		* static_cast<float>(TrunkActor->GetMaskWidth() + 2);
	Event.DamageShape.Thickness = TrunkActor->GetCellSize() * 1.25f;
	UFragmentSimulationSubsystem* Subsystem =
		World->GetSubsystem<UFragmentSimulationSubsystem>();
	if (!TestNotNull(TEXT("Fragment subsystem exists"), Subsystem)
		|| !TestTrue(
			TEXT("Burning tree can be felled"),
			Subsystem->RequestFragmentDamage(TrunkActor, Event)))
	{
		return false;
	}

	AFragment2DActor* Carrier = nullptr;
	for (TActorIterator<AFragment2DActor> It(World); It; ++It)
	{
		if (It->ContainsAggregateSource(Leaf->SourceId))
		{
			Carrier = *It;
			break;
		}
	}
	TestNotNull(
		TEXT("Burning leaf becomes logical state inside the physical carrier"),
		Carrier);
	bool bLeafActorRemains = false;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		bLeafActorRemains |= !It->IsActorBeingDestroyed()
			&& It->SourceId == Leaf->SourceId;
	}
	TestFalse(
		TEXT("Burning leaf does not fall back to an attached Source Actor"),
		bLeafActorRemains);

	for (int32 Step = 0; Step < 50; ++Step)
	{
		WorldActor->Tick(0.1f);
	}
	TestTrue(
		TEXT("Logical combustion continues after dynamic carrier handoff"),
		WorldActor->GetLogicalCombustionResidueCellCount(TEXT("leaf")) > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxDecorationCombustionIntegrationTest,
	"MatterFlux.Combustion.DecorationMaskBurnsThroughLuaRule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxDecorationCombustionIntegrationTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DSourceActor* Source =
		World ? World->SpawnActor<AFragment2DSourceActor>() : nullptr;
	if (!TestNotNull(TEXT("Fuel source actor spawns"), Source))
	{
		return false;
	}

	FFragmentSourceMask Mask;
	Mask.Width = 4;
	Mask.Height = 4;
	Mask.CellSize = 10.0f;
	Mask.MinFragmentAreaPixels = 1;
	Mask.MaxFragmentsPerBreak = 4;
	Mask.SolidMask.Init(1, 16);
	TestTrue(TEXT("Wood mask initializes with material identity"),
		Source->InitializeFromProceduralMask(
			Mask,
			FGuid::NewDeterministicGuid(TEXT("CombustionActorTest"), 1),
			FLinearColor(0.38f, 0.18f, 0.05f),
			TEXT("wood")));
	TestEqual(TEXT("Source retains Lua material identity"),
		Source->SourceMaterialId,
		FName(TEXT("wood")));

	const FVector LocalIgnitionPoint(-5.0f, 0.0f, -15.0f);
	TestTrue(TEXT("Configured fire material ignites wood"),
		Source->IgniteAtWorldLocation(
			Source->GetActorTransform().TransformPosition(
				LocalIgnitionPoint),
			TEXT("fire"),
			404));

	for (int32 Step = 0; Step < 80; ++Step)
	{
		Source->Tick(0.1f);
	}

	TestTrue(TEXT("Wood fuel mask is consumed"),
		Source->GetRemainingFuelCellCount() < 16);
	TestTrue(TEXT("Burned wood leaves solid residue"),
		Source->GetResidueCellCount() > 0);
	TestTrue(TEXT("Burning emitted visible smoke particles"),
		Source->GetTotalSmokeEmissionCount() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxForestPlantCombustionTest,
	"MatterFlux.Combustion.LeavesGrassAndFlowersUseConfiguredReactions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxForestPlantCombustionTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Combustion test world exists"), World))
	{
		return false;
	}
	static const FName FuelMaterials[] =
	{
		TEXT("leaf"),
		TEXT("grass"),
		TEXT("grassland"),
		TEXT("flower_pink"),
		TEXT("flower_gold"),
		TEXT("flower_blue")
	};
	for (int32 FuelIndex = 0;
		FuelIndex < UE_ARRAY_COUNT(FuelMaterials);
		++FuelIndex)
	{
		AFragment2DSourceActor* Source =
			World->SpawnActor<AFragment2DSourceActor>();
		if (!TestNotNull(
			*FString::Printf(
				TEXT("%s source spawns"),
				*FuelMaterials[FuelIndex].ToString()),
			Source))
		{
			return false;
		}
		FFragmentSourceMask Mask;
		Mask.Width = 2;
		Mask.Height = 2;
		Mask.CellSize = 8.0f;
		Mask.MinFragmentAreaPixels = 1;
		Mask.MaxFragmentsPerBreak = 4;
		Mask.SolidMask.Init(1, 4);
		TestTrue(
			*FString::Printf(
				TEXT("%s mask initializes"),
				*FuelMaterials[FuelIndex].ToString()),
			Source->InitializeFromProceduralMask(
				Mask,
				FGuid::NewDeterministicGuid(
					TEXT("ForestPlantCombustion"),
					FuelIndex + 1),
				FLinearColor::Green,
				FuelMaterials[FuelIndex]));
		TestTrue(
			*FString::Printf(
				TEXT("%s reacts with fire"),
				*FuelMaterials[FuelIndex].ToString()),
			Source->IgniteAtWorldLocation(
				Source->GetActorLocation(),
				TEXT("fire"),
				500 + FuelIndex));
		for (int32 Step = 0; Step < 30; ++Step)
		{
			Source->Tick(0.1f);
		}
		TestTrue(
			*FString::Printf(
				TEXT("%s leaves solid ash"),
				*FuelMaterials[FuelIndex].ToString()),
			Source->GetResidueCellCount() > 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxCombustionStateRoundTripTest,
	"MatterFlux.Combustion.StateSnapshotResumesDeterministically",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxCombustionStateRoundTripTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = 5;
	Mask.Height = 4;
	Mask.SolidMask.Init(1, Mask.Width * Mask.Height);

	FMatterFluxCombustionDefinition Rule;
	Rule.Id = TEXT("snapshot_fire");
	Rule.FuelMaterial = TEXT("wood");
	Rule.FlameMaterial = TEXT("fire");
	Rule.SmokeMaterial = TEXT("smoke");
	Rule.ResidueMaterial = TEXT("charcoal");
	Rule.IgnitionChancePermille = 1000;
	Rule.SpreadChancePermille = 730;
	Rule.SmokeChancePermille = 610;
	Rule.BurnDurationSteps = 7;

	MatterFlux::Combustion::FMaskCombustion Authority;
	TestTrue(TEXT("Authority combustion initializes"),
		Authority.Initialize(Mask, Rule, 4242));
	TestTrue(TEXT("Authority combustion ignites"),
		Authority.Ignite(FIntPoint(2, 1), TEXT("fire")));
	for (int32 Step = 0; Step < 4; ++Step)
	{
		Authority.Step();
	}

	MatterFlux::Combustion::FStateSnapshot Snapshot;
	TestTrue(TEXT("Combustion exports its complete deterministic state"),
		Authority.CaptureState(Snapshot));
	MatterFlux::Combustion::FMaskCombustion Restored;
	FString Error;
	TestTrue(TEXT("Combustion restores without replaying missed steps"),
		Restored.RestoreState(Snapshot, Rule, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
	}
	TestTrue(TEXT("Restored fuel is exact"),
		Restored.GetFuelMask() == Authority.GetFuelMask());
	TestTrue(TEXT("Restored residue is exact"),
		Restored.GetResidueMask() == Authority.GetResidueMask());
	TestTrue(TEXT("Restored burn durations are exact"),
		Restored.GetBurningMask() == Authority.GetBurningMask());

	for (int32 Step = 0; Step < 12; ++Step)
	{
		const MatterFlux::Combustion::FStepStats AuthorityStats =
			Authority.Step();
		const MatterFlux::Combustion::FStepStats RestoredStats =
			Restored.Step();
		TestEqual(TEXT("Resumed ignition count stays deterministic"),
			RestoredStats.IgnitedCells,
			AuthorityStats.IgnitedCells);
		TestEqual(TEXT("Resumed consumption count stays deterministic"),
			RestoredStats.ConsumedFuelCells,
			AuthorityStats.ConsumedFuelCells);
		TestTrue(TEXT("Resumed smoke cells stay deterministic"),
			RestoredStats.SmokeEmissionCells
				== AuthorityStats.SmokeEmissionCells);
	}
	return true;
}
