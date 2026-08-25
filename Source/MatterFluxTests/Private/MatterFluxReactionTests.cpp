#include "Material/MatterFluxReaction.h"
#include "Material/MatterFluxGroundReactionRuntime.h"
#include "Material/MatterFluxMaterialContactGeometry.h"
#include "Material/MatterFluxSourceReactionRuntime.h"
#include "Fragment/Fragment2DActor.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/Fragment2DSourceStreamingState.h"
#include "Fragment/FragmentGeometry.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Game/MatterFluxPlayableLevel.h"
#include "GAS/GA_PlayerFlameJet.h"
#include "IMatterFluxScriptRuntime.h"
#include "Magic/MatterFluxMagicProjectile.h"
#include "Components/SceneComponent.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Rendering/MatterFluxSmokeVisualPool.h"
#include "Save/MatterFluxSaveGame.h"
#include "Tests/AutomationEditorCommon.h"

#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterialContactGeometryPhaseTest,
	"MatterFlux.Reaction.MaterialContactGeometrySeparatesGasFromLiquid",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMaterialContactGeometryPhaseTest::RunTest(
	const FString& Parameters)
{
	constexpr float CellSize = 8.0f;
	constexpr float LiquidColumnHeight = 128.0f;
	const MatterFlux::Material::FMaterialContactGeometry Fire =
		MatterFlux::Material::BuildMaterialContactGeometry(
			EMatterFluxMaterialPhase::Gas,
			CellSize,
			LiquidColumnHeight);
	const MatterFlux::Material::FMaterialContactGeometry Acid =
		MatterFlux::Material::BuildMaterialContactGeometry(
			EMatterFluxMaterialPhase::Liquid,
			CellSize,
			LiquidColumnHeight);

	TestTrue(TEXT("Gas and liquid contact geometries are valid"),
		Fire.IsValid() && Acid.IsValid());
	TestEqual(TEXT("A fire cell uses one material-cell height"),
		Fire.ColumnHeight, CellSize);
	TestEqual(TEXT("A fire cell contact radius stays local"),
		Fire.RadialContactRadius, CellSize * 0.52f);
	TestEqual(TEXT("Fire cannot reach ground through liquid-column tolerance"),
		Fire.GroundVerticalTolerance, CellSize);
	TestEqual(TEXT("Acid retains the authored liquid column height"),
		Acid.ColumnHeight, LiquidColumnHeight);
	TestEqual(TEXT("Acid retains liquid-column contact radius"),
		Acid.RadialContactRadius, LiquidColumnHeight * 0.5f);
	TestEqual(TEXT("Acid retains liquid ground-contact tolerance"),
		Acid.GroundVerticalTolerance, LiquidColumnHeight);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxGroundRepeatedStimulusTest,
	"MatterFlux.Reaction.GroundRuntimeRepeatedContactDoesNotJumpToNeighbor",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxGroundRepeatedStimulusTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = 64;
	Mask.Height = 64;
	Mask.CellSize = 8.0f;
	Mask.SolidMask.Init(1, Mask.Width * Mask.Height);
	FMatterFluxReactionDefinition Rule;
	Rule.Id = TEXT("ground_repeated_fire_contact");
	Rule.InputA = TEXT("grassland");
	Rule.InputB = TEXT("fire");
	Rule.OutputA = TEXT("ash");
	Rule.ChancePermille = 1000;
	Rule.PropagationChancePermille = 0;
	Rule.DurationSteps = 8;

	MatterFlux::Reaction::FGroundRuntimeSettings Settings;
	Settings.Width = Mask.Width;
	Settings.Height = Mask.Height;
	MatterFlux::Reaction::FGroundReactionRuntime Runtime;
	FString Error;
	if (!TestTrue(TEXT("Ground runtime initializes"),
		Runtime.Initialize(Settings, Mask, Rule, 1337, Error)))
	{
		AddError(Error);
		return false;
	}

	const FIntPoint Contact(32, 32);
	FIntPoint ActivatedCell = FIntPoint::ZeroValue;
	TestTrue(TEXT("First fire contact activates its requested ground cell"),
		Runtime.ActivateNearestInput(Contact, TEXT("fire"), 5, ActivatedCell));
	TestEqual(TEXT("The requested cell is selected first"),
		ActivatedCell, Contact);
	TestFalse(TEXT("Repeated contact does not select a neighboring fuel cell"),
		Runtime.ActivateNearestInput(Contact, TEXT("fire"), 5, ActivatedCell));
	TArray<int32> ActiveCells;
	Runtime.GatherActiveCellIndices(ActiveCells);
	TestEqual(TEXT("Only one ground cell remains active"),
		ActiveCells.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxUnifiedSmokeVisualPoolTest,
	"MatterFlux.Reaction.Visuals.SharedSmokePoolBuildsPersistentVoxelClouds",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxUnifiedSmokeVisualPoolTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Rendering::FSmokeVisualSettings Settings;
	Settings.SpawnIntervalSeconds = 0.1f;
	Settings.MinimumLifetimeSeconds = 3.0f;
	Settings.MaximumLifetimeSeconds = 3.0f;
	Settings.MaximumParticles = 2;
	Settings.MaximumNewParticlesPerStep = 2;
	Settings.VoxelsPerParticle = 5;

	MatterFlux::Rendering::FSmokeVisualPool Pool;
	TestTrue(TEXT("Valid shared smoke settings are accepted"),
		Pool.Configure(Settings));
	MatterFlux::Rendering::FMaterialEmissionAnchor Anchor;
	Anchor.WorldPosition = FVector(100.0f, 200.0f, 300.0f);
	Anchor.CellSize = 20.0f;
	Anchor.EmissionProbability = 1.0f;
	Anchor.Seed = 1337;
	Pool.SetEmissionAnchors(MakeArrayView(&Anchor, 1));
	Pool.Advance(0.1f);

	TestEqual(TEXT("One anchor emits one bounded smoke particle per interval"),
		Pool.GetParticleCount(), 1);
	TArray<FTransform> ClusterTransforms;
	Pool.BuildInstanceTransforms(ClusterTransforms);
	TestEqual(TEXT("A smoke puff is a voxel cluster rather than one grey cube"),
		ClusterTransforms.Num(), Settings.VoxelsPerParticle);

	const FVector BeforeRise = Pool.GetParticlePosition(0);
	Pool.SetEmissionAnchors(TConstArrayView<
		MatterFlux::Rendering::FMaterialEmissionAnchor>());
	for (int32 Step = 0; Step < 20; ++Step)
	{
		Pool.Advance(0.1f);
	}
	TestEqual(TEXT("Smoke survives after its flame anchor disappears"),
		Pool.GetParticleCount(), 1);
	TestTrue(TEXT("Persistent smoke rises while it ages"),
		Pool.GetParticlePosition(0).Z > BeforeRise.Z);
	for (int32 Step = 0; Step < 11; ++Step)
	{
		Pool.Advance(0.1f);
	}
	TestEqual(TEXT("Smoke expires after the configured lifetime"),
		Pool.GetParticleCount(), 0);

	Pool.SetEmissionAnchors(MakeArrayView(&Anchor, 1));
	Pool.Advance(1.0f);
	TestEqual(TEXT("The world smoke budget is enforced"),
		Pool.GetParticleCount(), Settings.MaximumParticles);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSourceReactionRuntimeFixedStepTest,
	"MatterFlux.Reaction.SourceRuntimePreservesFixedStepDebtWithoutActor",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxSourceReactionRuntimeFixedStepTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = 4;
	Mask.Height = 1;
	Mask.CellSize = 4.0f;
	Mask.SolidMask.Init(1, Mask.Width * Mask.Height);

	FMatterFluxReactionDefinition Rule;
	Rule.Id = TEXT("source_runtime_fire");
	Rule.InputA = TEXT("wood");
	Rule.InputB = TEXT("fire");
	Rule.EmissionMaterial = TEXT("smoke");
	Rule.OutputA = TEXT("charcoal");
	Rule.ChancePermille = 1000;
	Rule.PropagationChancePermille = 1000;
	Rule.EmissionChancePermille = 1000;
	Rule.DurationSteps = 2;

	MatterFlux::Reaction::FSourceRuntimeSettings Settings;
	MatterFlux::Reaction::FSourceReactionRuntime Original;
	FString Error;
	TestTrue(
		TEXT("Source runtime initializes without a UObject"),
		Original.Initialize(Settings, Mask, Rule, 1337, Error));
	TestTrue(
		TEXT("Nearest input cell ignites deterministically"),
		Original.ActivateNearest(FIntPoint(-10, 0), TEXT("fire")));

	const MatterFlux::Reaction::FSourceAdvanceResult BeforeStep =
		Original.AdvanceAuthority(0.06f);
	TestEqual(TEXT("Sub-step time does not advance simulation"),
		BeforeStep.Steps,
		0);

	MatterFlux::Reaction::FSourceRuntimeSnapshot Snapshot;
	TestTrue(TEXT("Runtime captures fixed-step debt"),
		Original.CaptureState(Snapshot));
	TestTrue(TEXT("Snapshot retains the sub-step accumulator"),
		FMath::IsNearlyEqual(Snapshot.ReactionAccumulator, 0.06f));

	MatterFlux::Reaction::FSourceReactionRuntime Restored;
	TestTrue(TEXT("Runtime restores transactionally"),
		Restored.RestoreState(Settings, Snapshot, Rule, Error));
	const MatterFlux::Reaction::FSourceAdvanceResult OriginalStep =
		Original.AdvanceAuthority(0.04f);
	const MatterFlux::Reaction::FSourceAdvanceResult RestoredStep =
		Restored.AdvanceAuthority(0.04f);

	TestEqual(TEXT("Debt completes exactly one fixed step"),
		OriginalStep.Steps,
		1);
	TestEqual(TEXT("Restored runtime completes the same step count"),
		RestoredStep.Steps,
		OriginalStep.Steps);
	TestTrue(TEXT("Material emissions are deterministic"),
		RestoredStep.MaterialEmissionCells
			== OriginalStep.MaterialEmissionCells);
	TestTrue(TEXT("Changed cells are deterministic"),
		RestoredStep.ChangedCellIndices
			== OriginalStep.ChangedCellIndices);
	TestTrue(TEXT("Input state stays identical"),
		Restored.GetInputMask() == Original.GetInputMask());
	TestTrue(TEXT("Output state stays identical"),
		Restored.GetOutputMask() == Original.GetOutputMask());
	TestTrue(TEXT("Active state stays identical"),
		Restored.GetActiveMask() == Original.GetActiveMask());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSourceReactionFrontBudgetTest,
	"MatterFlux.Reaction.SourceRuntimeLimitsFireFrontPerFixedStep",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxSourceReactionFrontBudgetTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = 9;
	Mask.Height = 9;
	Mask.CellSize = 4.0f;
	Mask.SolidMask.Init(1, Mask.Width * Mask.Height);

	FMatterFluxReactionDefinition Rule;
	Rule.Id = TEXT("bounded_leaf_fire");
	Rule.InputA = TEXT("leaf");
	Rule.InputB = TEXT("fire");
	Rule.EmissionMaterial = TEXT("smoke");
	Rule.OutputA = TEXT("charcoal");
	Rule.ChancePermille = 1000;
	Rule.PropagationChancePermille = 1000;
	Rule.DurationSteps = 8;

	MatterFlux::Reaction::FSourceRuntimeSettings Settings;
	Settings.MaxActivationsPerStep = 1;
	MatterFlux::Reaction::FSourceReactionRuntime Runtime;
	FString Error;
	if (!TestTrue(
		TEXT("Dense leaf crown runtime initializes"),
		Runtime.Initialize(Settings, Mask, Rule, 2026, Error))
		|| !TestTrue(
			TEXT("Center leaf cell ignites"),
			Runtime.ActivateNearest(FIntPoint(4, 4), TEXT("fire"))))
	{
		AddError(Error);
		return false;
	}
	const MatterFlux::Reaction::FSourceAdvanceResult Result =
		Runtime.AdvanceAuthority(Settings.StepSeconds);
	int32 ActiveCells = 0;
	for (const uint8 Value : Runtime.GetActiveMask())
	{
		ActiveCells += Value != 0 ? 1 : 0;
	}
	TestEqual(TEXT("One fixed step adds only one new active cell"),
		ActiveCells,
		2);
	TestEqual(TEXT("The bounded front still performs one deterministic step"),
		Result.Steps,
		1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSourceReactionBudgetCompletesConnectedMaskTest,
	"MatterFlux.Reaction.SourceRuntimeBudgetDoesNotOrphanConnectedCells",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxSourceReactionBudgetCompletesConnectedMaskTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FFragmentSourceMask Mask;
	Mask.Width = 5;
	Mask.Height = 5;
	Mask.CellSize = 4.0f;
	Mask.SolidMask.Init(1, Mask.Width * Mask.Height);

	FMatterFluxReactionDefinition Rule;
	Rule.Id = TEXT("budgeted_connected_reaction");
	Rule.InputA = TEXT("leaf");
	Rule.InputB = TEXT("fire");
	Rule.OutputA = TEXT("ash");
	Rule.OutputB = TEXT("fire");
	Rule.EmissionMaterial = TEXT("smoke");
	Rule.ChancePermille = 1000;
	Rule.PropagationChancePermille = 1000;
	Rule.EmissionChancePermille = 0;
	Rule.DurationSteps = 3;

	MatterFlux::Reaction::FSourceRuntimeSettings Settings;
	Settings.MaxActivationsPerStep = 1;
	MatterFlux::Reaction::FSourceReactionRuntime Runtime;
	FString Error;
	if (!TestTrue(TEXT("Budgeted connected mask initializes"),
		Runtime.Initialize(Settings, Mask, Rule, 551, Error))
		|| !TestTrue(TEXT("Center cell accepts the generic stimulus"),
			Runtime.ActivateNearest(FIntPoint(2, 2), TEXT("fire"))))
	{
		AddError(Error);
		return false;
	}
	for (int32 Step = 0; Step < 256 && Runtime.IsActive(); ++Step)
	{
		Runtime.AdvanceAuthority(Settings.StepSeconds);
	}
	int32 RemainingInputCells = 0;
	for (const uint8 Cell : Runtime.GetInputMask())
	{
		RemainingInputCells += Cell != 0 ? 1 : 0;
	}
	TestFalse(TEXT("A finite connected reaction eventually completes"),
		Runtime.IsActive());
	TestEqual(TEXT("The front budget leaves no connected input island"),
		RemainingInputCells, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLeafFireUsesEdgeConnectedFrontTest,
	"MatterFlux.Reaction.LeafFireUsesEdgeConnectedFront",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxLeafFireUsesEdgeConnectedFrontTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = 2;
	Mask.Height = 2;
	Mask.CellSize = 4.0f;
	Mask.SolidMask = {
		1, 0,
		0, 1
	};

	FMatterFluxReactionDefinition Rule;
	Rule.Id = TEXT("edge_connected_leaf_fire");
	Rule.InputA = TEXT("leaf");
	Rule.InputB = TEXT("fire");
	Rule.EmissionMaterial = TEXT("smoke");
	Rule.OutputA = TEXT("ash");
	Rule.ChancePermille = 1000;
	Rule.PropagationChancePermille = 1000;
	Rule.DurationSteps = 8;

	MatterFlux::Reaction::FMaskReaction Simulation;
	if (!TestTrue(
		TEXT("Diagonal leaf fixture initializes"),
		Simulation.Initialize(Mask, Rule, 901))
		|| !TestTrue(
			TEXT("First leaf pixel ignites"),
			Simulation .Activate(FIntPoint(0, 0), TEXT("fire"))))
	{
		return false;
	}
	const MatterFlux::Reaction::FStepStats Step = Simulation.Step(1);
	TestEqual(
		TEXT("Fire cannot jump across a corner and create a disconnected flame point"),
		Step.ActivatedCells,
		0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSourceReactionSnapshotReuseTest,
	"MatterFlux.Reaction.SourceSnapshotCaptureReusesCallerStorageTransactionally",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxSourceReactionSnapshotReuseTest::RunTest(
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

	FMatterFluxReactionDefinition Rule;
	Rule.Id = TEXT("source_snapshot_reuse");
	Rule.InputA = TEXT("wood");
	Rule.InputB = TEXT("fire");
	Rule.EmissionMaterial = TEXT("smoke");
	Rule.OutputA = TEXT("charcoal");
	Rule.ChancePermille = 1000;
	Rule.PropagationChancePermille = 0;
	Rule.DurationSteps = 8;

	MatterFlux::Reaction::FSourceReactionRuntime Runtime;
	FString Error;
	if (!TestTrue(
		TEXT("Source runtime initializes for reusable capture"),
		Runtime.Initialize(
			MatterFlux::Reaction::FSourceRuntimeSettings(),
			Mask,
			Rule,
			4242,
			Error)))
	{
		AddError(Error);
		return false;
	}

	MatterFlux::Reaction::FSourceRuntimeSnapshot Snapshot;
	Snapshot.ReactionState.InputMask.Reserve(CellCount * 2);
	Snapshot.ReactionState.OutputMask.Reserve(CellCount * 2);
	Snapshot.ReactionState.ActiveMask.Reserve(CellCount * 2);
	const SIZE_T ReservedBytes =
		Snapshot.ReactionState.InputMask.GetAllocatedSize()
		+ Snapshot.ReactionState.OutputMask.GetAllocatedSize()
		+ Snapshot.ReactionState.ActiveMask.GetAllocatedSize();
	if (!TestTrue(
		TEXT("Valid capture succeeds"),
		Runtime.CaptureState(Snapshot)))
	{
		return false;
	}
	const SIZE_T CapturedBytes =
		Snapshot.ReactionState.InputMask.GetAllocatedSize()
		+ Snapshot.ReactionState.OutputMask.GetAllocatedSize()
		+ Snapshot.ReactionState.ActiveMask.GetAllocatedSize();
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

	const MatterFlux::Reaction::FSourceRuntimeSnapshot Committed = Snapshot;
	MatterFlux::Reaction::FSourceReactionRuntime InvalidRuntime;
	TestFalse(
		TEXT("An uninitialized runtime rejects capture"),
		InvalidRuntime.CaptureState(Snapshot));
	TestTrue(
		TEXT("Rejected capture preserves the committed reaction snapshot"),
		Snapshot.ReactionState.RuleId
			== Committed.ReactionState.RuleId
			&& Snapshot.ReactionState.Tick
				== Committed.ReactionState.Tick
			&& Snapshot.ReactionState.InputMask
				== Committed.ReactionState.InputMask
			&& Snapshot.ReactionState.OutputMask
				== Committed.ReactionState.OutputMask
			&& Snapshot.ReactionState.ActiveMask
				== Committed.ReactionState.ActiveMask
			&& Snapshot.ReactionAccumulator
				== Committed.ReactionAccumulator
			&& Snapshot.TotalMaterialEmissionCount
				== Committed.TotalMaterialEmissionCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSourceStreamingMaskStorageTest,
	"MatterFlux.Reaction.StreamingStateStoresOneCanonicalRuntimeMask",
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

	FMatterFluxReactionDefinition Rule;
	Rule.Id = TEXT("canonical_streaming_mask");
	Rule.InputA = TEXT("wood");
	Rule.InputB = TEXT("fire");
	Rule.EmissionMaterial = TEXT("smoke");
	Rule.OutputA = TEXT("charcoal");
	Rule.ChancePermille = 1000;
	Rule.PropagationChancePermille = 0;
	Rule.DurationSteps = 8;

	MatterFlux::Reaction::FSourceReactionRuntime Runtime;
	FString Error;
	if (!TestTrue(
		TEXT("Runtime initializes for canonical streaming capture"),
		Runtime.Initialize(
			MatterFlux::Reaction::FSourceRuntimeSettings(),
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
		TEXT("Streaming state captures the reaction runtime"),
		State.CaptureReactionState(Runtime)))
	{
		return false;
	}
	TestTrue(
		TEXT("Effective runtime mask is the captured input truth"),
		State.GetRuntimeMask() == Runtime.GetInputMask());
	TestEqual(
		TEXT("Reacting state stores input, output and active exactly once"),
		State.GetStoredMaskValueCount(),
		CellCount * 3);

	const FFragment2DSourceStreamingState Committed = State;
	MatterFlux::Reaction::FSourceReactionRuntime InvalidRuntime;
	TestFalse(
		TEXT("Invalid runtime capture is rejected"),
		State.CaptureReactionState(InvalidRuntime));
	TestTrue(
		TEXT("Rejected capture preserves canonical mask truth"),
		State.GetRuntimeMask() == Committed.GetRuntimeMask()
			&& State.GetStoredMaskValueCount()
				== Committed.GetStoredMaskValueCount());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaskReactionConsumesInputTest,
	"MatterFlux.Reaction.FireSpreadsAndProducesSmokeAndSolidOutput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxMaskReactionConsumesInputTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = 3;
	Mask.Height = 1;
	Mask.CellSize = 10.0f;
	Mask.MinFragmentAreaPixels = 1;
	Mask.MaxFragmentsPerBreak = 4;
	Mask.SolidMask.Init(1, 3);

	FMatterFluxReactionDefinition Rule;
	Rule.Id = TEXT("test_burn");
	Rule.InputA = TEXT("wood");
	Rule.InputB = TEXT("fire");
	Rule.EmissionMaterial = TEXT("smoke");
	Rule.OutputA = TEXT("charcoal");
	Rule.ChancePermille = 1000;
	Rule.PropagationChancePermille = 1000;
	Rule.DurationSteps = 2;
	Rule.EmissionChancePermille = 1000;

	MatterFlux::Reaction::FMaskReaction Simulation;
	TestTrue(
		TEXT("Valid input mask initializes"),
		Simulation.Initialize(Mask, Rule, 1337));
	TestTrue(
		TEXT("A flame ignites the selected input cell"),
		Simulation .Activate(FIntPoint(1, 0), TEXT("fire")));

	int32 TotalSmoke = 0;
	for (int32 Step = 0; Step < 5; ++Step)
	{
		TotalSmoke += Simulation.Step().MaterialEmissionCells.Num();
	}

	TestFalse(TEXT("Fire finishes after consuming connected input"),
		Simulation.IsActive());
	TestEqual(TEXT("Connected input is consumed"),
		Simulation.CountInputCells(),
		0);
	TestEqual(TEXT("Every consumed cell leaves solid output"),
		Simulation.CountOutputCells(),
		3);
	TestTrue(TEXT("Active emits smoke particles"), TotalSmoke > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxReactionDamageConstraintTest,
	"MatterFlux.Reaction.DamageCannotResurrectRemovedInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxReactionDamageConstraintTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = 3;
	Mask.Height = 1;
	Mask.CellSize = 10.0f;
	Mask.MinFragmentAreaPixels = 1;
	Mask.MaxFragmentsPerBreak = 4;
	Mask.SolidMask.Init(1, 3);

	FMatterFluxReactionDefinition Rule;
	Rule.Id = TEXT("damage_constraint");
	Rule.InputA = TEXT("wood");
	Rule.InputB = TEXT("fire");
	Rule.EmissionMaterial = TEXT("smoke");
	Rule.OutputA = TEXT("charcoal");
	Rule.ChancePermille = 1000;
	Rule.PropagationChancePermille = 1000;
	Rule.DurationSteps = 3;
	Rule.EmissionChancePermille = 0;

	MatterFlux::Reaction::FMaskReaction Simulation;
	TestTrue(TEXT("Input initializes"),
		Simulation.Initialize(Mask, Rule, 91));
	TestTrue(TEXT("Center input ignites"),
		Simulation .Activate(FIntPoint(1, 0), TEXT("fire")));

	TArray<uint8> DamageMask{0, 1, 1};
	TestTrue(TEXT("Committed damage constrains reaction input"),
		Simulation.ConstrainInputMask(DamageMask));
	for (int32 Step = 0; Step < 8; ++Step)
	{
		Simulation.Step();
	}
	TestEqual(TEXT("Removed input never returns"),
		Simulation.GetInputMask()[0],
		static_cast<uint8>(0));
	TestEqual(TEXT("Removed input does not become reaction output"),
		Simulation.GetOutputMask()[0],
		static_cast<uint8>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSourceReactionCutSynchronizationTest,
	"MatterFlux.Reaction.CutImmediatelySynchronizesVisibleAndReplicatedMasks",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxSourceReactionCutSynchronizationTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DSourceActor* Source = World
		? World->SpawnActor<AFragment2DSourceActor>()
		: nullptr;
	if (!TestNotNull(TEXT("Reactive source spawns"), Source))
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
				TEXT("ReactionCutSynchronization"),
				1),
			FLinearColor::White,
			TEXT("wood"))))
	{
		return false;
	}
	TestFalse(
		TEXT("Non-finite ignition location is rejected"),
		Source->ApplyMaterialStimulusAtWorldLocation(
			FVector(
				std::numeric_limits<double>::quiet_NaN(),
				0.0,
				0.0),
			TEXT("fire"),
			700));
	if (!TestTrue(
		TEXT("Finite ignition starts the source fire"),
		Source->ApplyMaterialStimulusAtWorldLocation(
			Source->GetActorLocation(),
			TEXT("fire"),
			701)))
	{
		return false;
	}
	TestEqual(
		TEXT("One visible cell is active before the cut"),
		Source->GetActiveCellCount(),
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
			TEXT("Cutting the active cell commits"),
			Subsystem->RequestFragmentDamage(Source, Event)))
	{
		return false;
	}

	TestEqual(
		TEXT("Cut clears visible active state immediately"),
		Source->GetActiveCellCount(),
		0);
	TestEqual(
		TEXT("Cut removes the reaction input"),
		Source->GetRemainingInputCellCount(),
		0);
	TestTrue(
		TEXT("Replicated reaction masks remain compact and synchronized"),
		Source->GetReplicatedReactionByteCount() <= 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLargeReactionMaskTest,
	"MatterFlux.Reaction.LargeSimulationMaskIsIndependentFromFragmentReplicationLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLargeReactionMaskTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = MatterFlux::PlayableLevel::TerrainCellsX;
	Mask.Height = MatterFlux::PlayableLevel::TerrainCellsY;
	Mask.SolidMask.Init(1, Mask.Width * Mask.Height);

	FMatterFluxReactionDefinition Rule;
	Rule.Id = TEXT("large_ground_burn");
	Rule.InputA = TEXT("grassland");
	Rule.InputB = TEXT("fire");
	Rule.EmissionMaterial = TEXT("smoke");
	Rule.OutputA = TEXT("ash");
	Rule.ChancePermille = 1000;
	Rule.PropagationChancePermille = 25;
	Rule.DurationSteps = 8;
	Rule.EmissionChancePermille = 420;

	MatterFlux::Reaction::FMaskReaction Simulation;
	TestTrue(
		TEXT("Large local simulation masks are not constrained by fragment replication dimensions"),
		Simulation.Initialize(Mask, Rule, 1337));
	TestTrue(
		TEXT("The initialized large mask can ignite"),
		Simulation .Activate(
			FIntPoint(Mask.Width / 2, Mask.Height / 2),
			TEXT("fire")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxGroundReactionRuntimeChunkBatchTest,
	"MatterFlux.Reaction.GroundRuntimeBatchesOnlyChangedChunks",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxGroundReactionRuntimeChunkBatchTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = 128;
	Mask.Height = 64;
	Mask.CellSize = 10.0f;
	Mask.SolidMask.Init(1, Mask.Width * Mask.Height);

	FMatterFluxReactionDefinition Rule;
	Rule.Id = TEXT("ground_runtime_chunk_batch");
	Rule.InputA = TEXT("grassland");
	Rule.InputB = TEXT("fire");
	Rule.EmissionMaterial = TEXT("smoke");
	Rule.OutputA = TEXT("ash");
	Rule.ChancePermille = 1000;
	Rule.PropagationChancePermille = 0;
	Rule.DurationSteps = 2;
	Rule.EmissionChancePermille = 0;

	MatterFlux::Reaction::FGroundRuntimeSettings Settings;
	Settings.Width = Mask.Width;
	Settings.Height = Mask.Height;
	MatterFlux::Reaction::FGroundReactionRuntime Runtime;
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
		Runtime.Activate(FIntPoint(65, 10), Rule.InputB));
	TArray<int32> ActiveCellIndices;
	Runtime.GatherActiveCellIndices(ActiveCellIndices);
	TestEqual(
		TEXT("The runtime exposes one sparse active cell after ignition"),
		ActiveCellIndices.Num(),
		1);
	if (ActiveCellIndices.Num() == 1)
	{
		TestEqual(
			TEXT("The sparse cell index matches the activated coordinate"),
			ActiveCellIndices[0],
			10 * Mask.Width + 65);
	}
	TArray<FIntPoint> ActiveChunks;
	Runtime.GatherActiveChunkCoordinates(ActiveChunks);
	TestTrue(
		TEXT("Sparse active cells collapse to one deterministic active chunk"),
		ActiveChunks == TArray<FIntPoint>({ FIntPoint(1, 0) }));
	TestTrue(
		TEXT("A second active cell in the same chunk is accepted"),
		Runtime.Activate(FIntPoint(66, 10), Rule.InputB));
	Runtime.GatherActiveChunkCoordinates(ActiveChunks);
	TestTrue(
		TEXT("Multiple active cells in one chunk still produce one query region"),
		ActiveChunks == TArray<FIntPoint>({ FIntPoint(1, 0) }));
	const TArray<FIntPoint> VisibleChunks = { FIntPoint(1, 0) };
	TArray<int32> VisibleOutputCells;
	TArray<int32> VisibleActiveCells;
	Runtime.GatherVisibleCellIndicesForChunks(
		VisibleChunks,
		VisibleOutputCells,
		VisibleActiveCells);
	TestTrue(
		TEXT("Visible chunk lookup returns only its current active cells"),
		VisibleOutputCells.IsEmpty()
			&& VisibleActiveCells == TArray<int32>(
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
	Runtime.GatherActiveCellIndices(ActiveCellIndices);
	Runtime.GatherActiveChunkCoordinates(ActiveChunks);
	TestEqual(
		TEXT("Sparse active cells are removed after fixed-step input exhaustion"),
		ActiveCellIndices.Num(),
		0);
	TestTrue(
		TEXT("Extinguished ground leaves no active reaction chunks"),
		ActiveChunks.IsEmpty());
	TArray<int32> OutputCellIndices;
	Runtime.GatherOutputCellIndices(OutputCellIndices);
	TestTrue(
		TEXT("Burned-out cells are exposed as a stable sparse output set"),
		OutputCellIndices == TArray<int32>(
			{ 10 * Mask.Width + 65, 10 * Mask.Width + 66 }));
	Runtime.GatherVisibleCellIndicesForChunks(
		VisibleChunks,
		VisibleOutputCells,
		VisibleActiveCells);
	TestTrue(
		TEXT("Visible chunk lookup replaces exhausted fire with output"),
		VisibleActiveCells.IsEmpty()
			&& VisibleOutputCells == OutputCellIndices);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxGroundReactionRuntimeReplicationGuardTest,
	"MatterFlux.Reaction.GroundRuntimeRejectsCorruptionAtomically",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxGroundReactionRuntimeReplicationGuardTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = 64;
	Mask.Height = 64;
	Mask.CellSize = 10.0f;
	Mask.SolidMask.Init(1, Mask.Width * Mask.Height);

	FMatterFluxReactionDefinition Rule;
	Rule.Id = TEXT("ground_runtime_replication_guard");
	Rule.InputA = TEXT("grassland");
	Rule.InputB = TEXT("fire");
	Rule.EmissionMaterial = TEXT("smoke");
	Rule.OutputA = TEXT("ash");
	Rule.ChancePermille = 1000;
	Rule.PropagationChancePermille = 0;
	Rule.DurationSteps = 2;

	MatterFlux::Reaction::FGroundRuntimeSettings Settings;
	Settings.Width = Mask.Width;
	Settings.Height = Mask.Height;
	MatterFlux::Reaction::FGroundReactionRuntime Authority;
	MatterFlux::Reaction::FGroundReactionRuntime Client;
	FString Error;
	if (!TestTrue(TEXT("Authority runtime initializes"),
		Authority.Initialize(Settings, Mask, Rule, 71, Error))
		|| !TestTrue(TEXT("Client runtime initializes"),
			Client.Initialize(Settings, Mask, Rule, 71, Error)))
	{
		AddError(Error);
		return false;
	}
	Authority .Activate(FIntPoint(2, 3), Rule.InputB);
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
	const TArray<uint8> BeforeOutput = Client.GetOutputMask();
	const TArray<uint8> BeforeActive = Client.GetActiveMask();
	TestEqual(
		TEXT("A corrupt chunk is rejected"),
		Client.ApplyReplicatedChunk(Corrupt, Error),
		MatterFlux::Reaction::EGroundChunkApplyResult::Rejected);
	TestTrue(
		TEXT("Rejected output is not partially applied"),
		Client.GetOutputMask() == BeforeOutput);
	TestTrue(
		TEXT("Rejected active state is not partially applied"),
		Client.GetActiveMask() == BeforeActive);
	TestEqual(
		TEXT("The valid payload applies after a corrupt payload at the same revision"),
		Client.ApplyReplicatedChunk(Batch[0], Error),
		MatterFlux::Reaction::EGroundChunkApplyResult::Applied);
	TArray<int32> AuthorityActiveCells;
	TArray<int32> ClientActiveCells;
	Authority.GatherActiveCellIndices(AuthorityActiveCells);
	Client.GatherActiveCellIndices(ClientActiveCells);
	TestTrue(
		TEXT("Replicated sparse active cells match authority"),
		ClientActiveCells == AuthorityActiveCells);
	TestEqual(
		TEXT("A repeated payload is idempotent"),
		Client.ApplyReplicatedChunk(Batch[0], Error),
		MatterFlux::Reaction::EGroundChunkApplyResult::NoChange);

	MatterFlux::Reaction::FGroundRuntimeSnapshot WrapSnapshot;
	if (!TestTrue(TEXT("Authority state captures for wrap testing"),
		Authority.CaptureState(WrapSnapshot)))
	{
		return false;
	}
	WrapSnapshot.Revision = MAX_int32;
	MatterFlux::Reaction::FGroundReactionRuntime WrappedAuthority;
	MatterFlux::Reaction::FGroundReactionRuntime WrappedClient;
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
		MatterFlux::Reaction::EGroundChunkApplyResult::Applied);
	WrappedAuthority .Activate(FIntPoint(5, 5), Rule.InputB);
	WrappedAuthority.BuildPendingReplication(WrappedBatch, Error);
	TestEqual(
		TEXT("Revision wraps from MAX_int32 to zero"),
		WrappedBatch[0].Revision,
		0);
	TestEqual(
		TEXT("Client accepts the wrapped revision as newer"),
		WrappedClient.ApplyReplicatedChunk(WrappedBatch[0], Error),
		MatterFlux::Reaction::EGroundChunkApplyResult::Applied);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxGroundReactionRuntimeSnapshotDebtTest,
	"MatterFlux.Reaction.GroundRuntimeSnapshotPreservesFixedStepDebt",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxGroundReactionRuntimeSnapshotDebtTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = 64;
	Mask.Height = 64;
	Mask.CellSize = 10.0f;
	Mask.SolidMask.Init(1, Mask.Width * Mask.Height);

	FMatterFluxReactionDefinition Rule;
	Rule.Id = TEXT("ground_runtime_snapshot_debt");
	Rule.InputA = TEXT("grassland");
	Rule.InputB = TEXT("fire");
	Rule.EmissionMaterial = TEXT("smoke");
	Rule.OutputA = TEXT("ash");
	Rule.ChancePermille = 1000;
	Rule.PropagationChancePermille = 0;
	Rule.DurationSteps = 1;

	MatterFlux::Reaction::FGroundRuntimeSettings Settings;
	Settings.Width = Mask.Width;
	Settings.Height = Mask.Height;
	MatterFlux::Reaction::FGroundReactionRuntime Original;
	FString Error;
	if (!TestTrue(TEXT("Original runtime initializes"),
		Original.Initialize(Settings, Mask, Rule, 91, Error)))
	{
		AddError(Error);
		return false;
	}
	Original .Activate(FIntPoint(4, 5), Rule.InputB);
	TestEqual(
		TEXT("Half a fixed step performs no simulation step"),
		Original.AdvanceAuthority(Settings.StepSeconds * 0.5f).Steps,
		0);
	MatterFlux::Reaction::FGroundRuntimeSnapshot Snapshot;
	if (!TestTrue(TEXT("Runtime snapshot captures scheduler debt"),
		Original.CaptureState(Snapshot)))
	{
		return false;
	}
	MatterFlux::Reaction::FGroundReactionRuntime Restored;
	if (!TestTrue(TEXT("Runtime snapshot restores atomically"),
		Restored.RestoreState(Settings, Snapshot, Rule, Error)))
	{
		AddError(Error);
		return false;
	}
	TArray<int32> OriginalActiveCells;
	TArray<int32> RestoredActiveCells;
	Original.GatherActiveCellIndices(OriginalActiveCells);
	Restored.GatherActiveCellIndices(RestoredActiveCells);
	TestTrue(
		TEXT("Restored sparse active cells match the captured runtime"),
		RestoredActiveCells == OriginalActiveCells
			&& !RestoredActiveCells.IsEmpty());
	TestEqual(
		TEXT("The restored half-step completes exactly one simulation step"),
		Restored.AdvanceAuthority(Settings.StepSeconds * 0.5f).Steps,
		1);
	Original.AdvanceAuthority(Settings.StepSeconds * 0.5f);
	TestTrue(
		TEXT("Restored state matches uninterrupted execution"),
		Restored.GetOutputMask() == Original.GetOutputMask());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSourceReactionReplicationBudgetTest,
	"MatterFlux.Reaction.SourceReplicationBitPacksAllMasks",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxSourceReactionReplicationBudgetTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DSourceActor* Source = World
		? World->SpawnActor<AFragment2DSourceActor>()
		: nullptr;
	if (!TestNotNull(TEXT("Reactive source spawns"), Source))
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
				TEXT("ReactionReplicationBudget"),
				1),
			FLinearColor::White,
			TEXT("wood"))))
	{
		return false;
	}
	if (!TestTrue(TEXT("Maximum source can ignite"),
		Source->ApplyMaterialStimulusAtWorldLocation(
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
		Source->GetReplicatedReactionByteCount()
			<= MaximumPackedBytes);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterialParticleActivatesLogicalSourceTest,
	"MatterFlux.Material.Interactions.PropagatingParticleActivatesLogicalSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxMaterialParticleActivatesLogicalSourceTest::RunTest(
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
	if (!TestNotNull(TEXT("Generated tree trunk exists"), Trunk))
	{
		return false;
	}
	const FVector ParticleLocation =
		(Trunk->Transform * WorldActor->GetActorTransform()).GetLocation();
	if (!TestTrue(
		TEXT("Stimulus material enters the material world"),
		WorldActor->SetSimulatedMaterialAtWorldLocation(
			ParticleLocation,
			TEXT("fire"))))
	{
		return false;
	}
	TestEqual(
		TEXT("Submitting a particle does not mutate a source synchronously"),
		WorldActor->GetReactingSourceCount(),
		0);

	WorldActor->Tick(0.06f);
	TestTrue(
		TEXT("The material interaction step activates the matching source rule"),
		WorldActor->GetReactingSourceCount() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxActiveReactionSaveValidationTest,
	"MatterFlux.Save.ActiveLogicalSourceReactionValidatesImmediately",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxActiveReactionSaveValidationTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor =
		World ? World->SpawnActor<AMatterFluxPlayableWorldActor>() : nullptr;
	if (!TestNotNull(TEXT("Playable world spawns"), WorldActor))
	{
		return false;
	}
	WorldActor->Regenerate(1337);
	if (!TestTrue(
		TEXT("A generated tree reaction can be activated"),
		WorldActor->ApplyMaterialStimulusToFirstGeneratedTree(991)))
	{
		return false;
	}

	FMatterFluxWorldSaveState WorldState;
	FString Error;
	if (!TestTrue(
		TEXT("The active reaction can be captured immediately"),
		WorldActor->CaptureSaveState(WorldState, Error)))
	{
		AddError(Error);
		return false;
	}

	UMatterFluxSaveGame* Save = NewObject<UMatterFluxSaveGame>();
	Save->InitializeNew(1337);
	Save->WorldState = MoveTemp(WorldState);
	const bool bValid = Save->ValidateAndMigrate(Error);
	if (!bValid)
	{
		AddError(Error);
	}
	TestTrue(TEXT("The active reaction produces a valid save"), bValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPlayableTreeReactionTest,
	"MatterFlux.Reaction.PlayableTreeBurnsFromTrunkIntoCanopy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxPlayableTreeReactionTest::RunTest(
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
			TEXT("mf.Reaction.ActivateTree")));
	TestTrue(TEXT("A generated tree trunk can be activated"),
		WorldActor->ApplyMaterialStimulusToFirstGeneratedTree(991));
	int32 MaterializedSourcesAfterIgnition = 0;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		MaterializedSourcesAfterIgnition +=
			!It->IsActorBeingDestroyed() ? 1 : 0;
	}
	TestEqual(
		TEXT("Static reaction remains logical instead of allocating Source Actors"),
		MaterializedSourcesAfterIgnition,
		0);

	for (int32 Step = 0; Step < 60; ++Step)
	{
		WorldActor->Tick(0.1f);
	}

	const int32 BurnedWoodCells =
		WorldActor->GetLogicalReactionOutputCellCount(TEXT("wood"));
	const int32 BurnedLeafCells =
		WorldActor->GetLogicalReactionOutputCellCount(TEXT("leaf"));
	const int32 MaterialEmissions =
		WorldActor->GetLogicalReactionMaterialEmissionCount();
	TestTrue(TEXT("The trunk becomes charcoal"), BurnedWoodCells > 0);
	TestTrue(TEXT("Fire crosses source boundaries into the canopy"),
		BurnedLeafCells > 0);
	TestTrue(TEXT("The active tree generates smoke particles"),
		MaterialEmissions > 0);
	TestEqual(
		TEXT("Burned proxy cells never overlap a resurrected pristine tree"),
		WorldActor->GetLogicalReactionProjectionOverlapCellCount(),
		0);
	TestEqual(
		TEXT("Burned tree material is part of the unified voxel object"),
		WorldActor->GetStandaloneTreeReactionOutputProjectionCount(),
		0);
	TestEqual(
		TEXT("Wood flames are gone after the six-second burn window"),
		WorldActor->GetLogicalReactionActiveCellCount(TEXT("wood")),
		0);
	TestEqual(
		TEXT("Leaf flames are gone after the six-second burn window"),
		WorldActor->GetLogicalReactionActiveCellCount(TEXT("leaf")),
		0);
	TestTrue(TEXT("Fire scorches the grassland beneath the tree"),
		WorldActor->GetReactedGroundCellCount() > 0);
	TestTrue(TEXT("A single tree fire does not instantly consume the map"),
		WorldActor->GetReactedGroundCellCount()
			< MatterFlux::PlayableLevel::TerrainCellsX
				* MatterFlux::PlayableLevel::TerrainCellsY / 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPlayableTreeTopCanopyReactionTest,
	"MatterFlux.Reaction.PlayableTreeConsumesTopCanopy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxPlayableTreeTopCanopyReactionTest::RunTest(
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
	if (!TestTrue(TEXT("Reference forest layout builds"),
		MatterFlux::PlayableLevel::BuildLevelLayout(
			1337,
			Layout,
			Registry.Get())))
	{
		return false;
	}

	const MatterFlux::PlayableLevel::FLevelFragmentSource* NearestWood =
		nullptr;
	double BestDistanceSquared = TNumericLimits<double>::Max();
	FString BestId;
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
		: Layout.FragmentSources)
	{
		if (Source.MaterialId != TEXT("wood") || !Source.Mask.IsValid())
		{
			continue;
		}
		const double DistanceSquared = FVector::DistSquared(
			Source.Transform.GetLocation(),
			FVector::ZeroVector);
		const FString CandidateId =
			Source.SourceId.ToString(EGuidFormats::Digits);
		if (!NearestWood || DistanceSquared < BestDistanceSquared
			|| (DistanceSquared == BestDistanceSquared
				&& CandidateId < BestId))
		{
			NearestWood = &Source;
			BestDistanceSquared = DistanceSquared;
			BestId = CandidateId;
		}
	}
	if (!TestNotNull(TEXT("Nearest generated tree wood exists"), NearestWood))
	{
		return false;
	}

	TArray<const MatterFlux::PlayableLevel::FLevelFragmentSource*>
		TopLeafSources;
	double HighestLeafZ = -TNumericLimits<double>::Max();
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
		: Layout.FragmentSources)
	{
		if (Source.AggregateId != NearestWood->AggregateId
			|| Source.MaterialId != TEXT("leaf")
			|| !Source.Mask.IsValid())
		{
			continue;
		}
		const FVector HalfExtent(
			Source.Mask.Width * Source.Mask.CellSize * 0.5f,
			Source.Mask.CellSize,
			Source.Mask.Height * Source.Mask.CellSize * 0.5f);
		const FBox WorldBounds = FBox(-HalfExtent, HalfExtent).TransformBy(
			Source.Transform.ToMatrixWithScale());
		if (WorldBounds.Max.Z > HighestLeafZ + KINDA_SMALL_NUMBER)
		{
			HighestLeafZ = WorldBounds.Max.Z;
			TopLeafSources.Reset();
		}
		if (FMath::IsNearlyEqual(WorldBounds.Max.Z, HighestLeafZ))
		{
			TopLeafSources.Add(&Source);
		}
	}
	if (!TestTrue(TEXT("The nearest tree has a highest leaf layer"),
		!TopLeafSources.IsEmpty()))
	{
		return false;
	}

	bool bEveryTopLeafBurned = true;
	constexpr int32 EventSeeds[] = { 1, 7, 19, 73 };
	for (const int32 EventSeed : EventSeeds)
	{
		WorldActor->Regenerate(1337);
		if (!TestTrue(TEXT("The generated tree accepts fire stimulus"),
			WorldActor->ApplyMaterialStimulusToFirstGeneratedTree(EventSeed)))
		{
			return false;
		}
		for (int32 Step = 0; Step < 60; ++Step)
		{
			WorldActor->Tick(0.1f);
		}

		for (const MatterFlux::PlayableLevel::FLevelFragmentSource* Source
			: TopLeafSources)
		{
			int32 Revision = INDEX_NONE;
			TArray<uint8> RuntimeMask;
			if (!Source)
			{
				return false;
			}
			if (!WorldActor->GetFragmentSourceRuntimeState(
				Source->SourceId,
				Revision,
				RuntimeMask))
			{
				continue;
			}
			int32 RemainingCells = 0;
			FString RemainingCoordinates;
			for (int32 CellIndex = 0; CellIndex < RuntimeMask.Num(); ++CellIndex)
			{
				if (RuntimeMask[CellIndex] == 0)
				{
					continue;
				}
				++RemainingCells;
				RemainingCoordinates += FString::Printf(
					TEXT(" (%d,%d)"),
					CellIndex % Source->Mask.Width,
					CellIndex / Source->Mask.Width);
			}
			if (RemainingCells > 0)
			{
				bEveryTopLeafBurned = false;
				AddError(FString::Printf(
					TEXT("Fire seed %d left %d cells%s in top leaf source %s at %s"),
					EventSeed,
					RemainingCells,
					*RemainingCoordinates,
					*Source->SourceId.ToString(EGuidFormats::Digits),
					*Source->Transform.GetLocation().ToCompactString()));
			}
		}
	}
	TestTrue(TEXT("Fire consumes every top-canopy leaf cell"),
		bEveryTopLeafBurned);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterializedFireKeepsLogicalNeighborsTest,
	"MatterFlux.Reaction.MaterializedFireKeepsLogicalNeighborsUnmaterialized",
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
	const FVector LowerTrunkPoint =
		(Trunk->Transform * WorldActor->GetActorTransform())
			.TransformPosition(FVector(
				0.0f,
				0.0f,
				-Trunk->Mask.Height * Trunk->Mask.CellSize * 0.3f));
	const FBox TrunkSelectionBounds = FBox::BuildAABB(
		LowerTrunkPoint,
		FVector(Trunk->Mask.CellSize * 0.45f));
	TArray<AFragment2DSourceActor*> Materialized;
	WorldActor->GatherFragmentSourcesInBounds(
		TrunkSelectionBounds,
		Materialized);
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
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
		: Layout.FragmentSources)
	{
		if (Source.SourceId != Trunk->SourceId)
		{
			WorldActor->DematerializeFragmentSource(Source.SourceId);
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

	const FVector StimulusPoint =
		TrunkBounds.GetClosestPointTo(LeafBounds.GetCenter());
	if (!TestTrue(
		TEXT("Materialized trunk ignites beside its logical canopy"),
		TrunkActor->ApplyMaterialStimulusAtWorldLocation(
			StimulusPoint,
			TEXT("fire"),
			923)))
	{
		return false;
	}
	const int32 ReactingBeforePropagation =
		WorldActor->GetReactingSourceCount();
	WorldActor->Tick(0.21f);
	WorldActor->Tick(0.06f);

	TestTrue(
		TEXT("Fire propagates from the Actor into a logical source"),
		WorldActor->GetReactingSourceCount()
			> ReactingBeforePropagation);
	TestEqual(
		TEXT("Propagation does not materialize logical neighbors"),
		WorldActor->GetGeneratedFragmentSourceCount(),
		MaterializedBeforePropagation);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterializedSourceReactionHandoffTest,
	"MatterFlux.Reaction.MaterializedStaticSourceReturnsToLogicalRuntime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxMaterializedSourceReactionHandoffTest::RunTest(
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

	TArray<AFragment2DSourceActor*> MaterializedSources;
	WorldActor->GatherFragmentSourcesInBounds(
		FBox(
			FVector(-100000.0f),
			FVector(100000.0f)),
		MaterializedSources);
	const int32 MaterializedCount = MaterializedSources.Num();
	if (!TestTrue(TEXT("Reactive sources can be materialized for interaction"),
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
		WoodSource->ApplyMaterialStimulusAtWorldLocation(
			WoodSource->GetActorLocation(),
			TEXT("fire"),
			731));
	TestTrue(TEXT("The static trunk returns to the logical store"),
		WorldActor->DematerializeFragmentSource(SourceId));
	TestEqual(TEXT("Reaction remains active after Actor handoff"),
		WorldActor->GetReactingSourceCount(),
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
	TestTrue(TEXT("Logical reaction continues producing output"),
		WorldActor->GetLogicalReactionOutputCellCount(TEXT("wood")) > 0);
	TestTrue(TEXT("Logical reaction continues emitting smoke"),
		WorldActor->GetLogicalReactionMaterialEmissionCount() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxActiveAggregateMemberHandoffTest,
	"MatterFlux.Reaction.ActiveTreeMemberMovesIntoOneDynamicCarrier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxActiveAggregateMemberHandoffTest::RunTest(
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
		WorldActor->ApplyMaterialStimulusToLogicalFragmentSourcesInBounds(
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
			TEXT("Active tree can be felled"),
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
		TEXT("Active leaf becomes logical state inside the physical carrier"),
		Carrier);
	bool bLeafActorRemains = false;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		bLeafActorRemains |= !It->IsActorBeingDestroyed()
			&& It->SourceId == Leaf->SourceId;
	}
	TestFalse(
		TEXT("Active leaf does not fall back to an attached Source Actor"),
		bLeafActorRemains);

	for (int32 Step = 0; Step < 50; ++Step)
	{
		WorldActor->Tick(0.1f);
	}
	TestTrue(
		TEXT("Logical reaction continues after dynamic carrier handoff"),
		WorldActor->GetLogicalReactionOutputCellCount(TEXT("leaf")) > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxDetachedWoodGroundIgnitionTest,
	"MatterFlux.Reaction.DetachedWoodActivatesFromActiveGround",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxDetachedWoodGroundIgnitionTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor =
		World ? World->SpawnActor<AMatterFluxPlayableWorldActor>() : nullptr;
	if (!TestNotNull(TEXT("Playable world spawns"), WorldActor))
	{
		return false;
	}
	WorldActor->Regenerate(1337);

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
	const MatterFlux::PlayableLevel::FLevelLayer* Stream =
		Layout.FindLayer(TEXT("Stream"));
	if (!TestNotNull(TEXT("Reference stream layer exists"), Stream))
	{
		return false;
	}
	TSet<int32> StreamCells;
	for (const FTransform& Transform : Stream->Instances)
	{
		const FVector Location = Transform.GetLocation();
		const int32 X = FMath::RoundToInt(
			(Location.X - Layout.Terrain.FirstCellCenter.X)
				/ Layout.Terrain.CellSize);
		const int32 Y = FMath::RoundToInt(
			(Location.Y - Layout.Terrain.FirstCellCenter.Y)
				/ Layout.Terrain.CellSize);
		if (X >= 0 && X < Layout.Terrain.Width
			&& Y >= 0 && Y < Layout.Terrain.Height)
		{
			StreamCells.Add(Layout.Terrain.ToIndex(X, Y));
		}
	}
	FIntPoint DryCell(INDEX_NONE, INDEX_NONE);
	for (int32 Y = Layout.Terrain.Height / 4;
		Y < Layout.Terrain.Height * 3 / 4 && DryCell.X == INDEX_NONE;
		++Y)
	{
		for (int32 X = Layout.Terrain.Width / 4;
			X < Layout.Terrain.Width * 3 / 4;
			++X)
		{
			if (!StreamCells.Contains(Layout.Terrain.ToIndex(X, Y)))
			{
				DryCell = FIntPoint(X, Y);
				break;
			}
		}
	}
	if (!TestTrue(TEXT("Reference terrain contains a dry ground cell"),
		DryCell.X != INDEX_NONE))
	{
		return false;
	}
	const FVector GroundCellWorld(
		Layout.Terrain.FirstCellCenter.X
			+ DryCell.X * Layout.Terrain.CellSize,
		Layout.Terrain.FirstCellCenter.Y
			+ DryCell.Y * Layout.Terrain.CellSize,
		Layout.Terrain.HeightAt(DryCell.X, DryCell.Y));

	constexpr float CellSize = 18.0f;
	MatterFlux::FragmentGeometry::FFragmentComponent Component;
	Component.Min = Component.Max = FIntPoint::ZeroValue;
	Component.Cells = {FIntPoint::ZeroValue};
	TArray<FFragmentSpawnPayload> Payloads;
	const FTransform SourceTransform(
		FVector(
			GroundCellWorld.X,
			GroundCellWorld.Y,
			GroundCellWorld.Z + CellSize * 0.5f));
	if (!TestTrue(
		TEXT("Detached voxel wood payload builds"),
		MatterFlux::FragmentGeometry::BuildSpawnPayloadsFromComponents(
			{Component},
			FGuid::NewDeterministicGuid(TEXT("GroundIgnitionWood"), 1),
			SourceTransform,
			1,
			1,
			1,
			CellSize,
			1,
			1,
			SourceTransform.GetLocation(),
			0.0f,
			771,
			Payloads,
			EFragmentSourceGeometryStyle::VoxelBlocks))
		|| !TestEqual(TEXT("One detached wood payload exists"), Payloads.Num(), 1))
	{
		return false;
	}
	Payloads[0].MaterialId = TEXT("wood");
	Payloads[0].bEnableCollision = false;
	AFragment2DActor* Carrier = World->SpawnActor<AFragment2DActor>();
	if (!TestNotNull(TEXT("Detached wood carrier spawns"), Carrier)
		|| !TestTrue(
			TEXT("Detached wood carrier initializes"),
			Carrier->InitializeFromPayload(Payloads[0])))
	{
		return false;
	}

	const int32 GroundOutputBefore =
		WorldActor->GetReactedGroundCellCount();
	TestTrue(
		TEXT("A material particle is deposited beside detached wood"),
		WorldActor->SetSimulatedMaterialAtWorldLocation(
			GroundCellWorld,
			TEXT("fire")));
	TestFalse(
		TEXT("Depositing a particle does not synchronously mutate the carrier"),
		Carrier->IsRootReacting());
	TestEqual(
		TEXT("Depositing a particle does not synchronously mutate ground"),
		WorldActor->GetReactedGroundCellCount(),
		GroundOutputBefore);
	WorldActor->Tick(0.06f);
	TestTrue(
		TEXT("The shared material step activates the ground adapter"),
		WorldActor->GetActiveGroundReactionCellCount() > 0);
	TestTrue(
		TEXT("The same material step activates detached wood"),
		Carrier->IsRootReacting());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxDetachedTreeIgnitionTest,
	"MatterFlux.Reaction.DetachedTreeWoodAndLeavesCanActivateAfterFelling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxDetachedTreeIgnitionTest::RunTest(
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
	const MatterFlux::PlayableLevel::FLevelFragmentSource* Leaf = Trunk
		? Layout.FragmentSources.FindByPredicate(
			[Trunk](const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
			{
				return Source.AggregateId == Trunk->AggregateId
					&& Source.MaterialId == TEXT("leaf")
					&& !Source.bAggregateRoot;
			})
		: nullptr;
	if (!TestNotNull(TEXT("Generated tree trunk exists"), Trunk)
		|| !TestNotNull(TEXT("Generated tree leaf exists"), Leaf))
	{
		return false;
	}
	const auto BuildWorldBounds = [WorldActor](
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
	Event.EventSeed = 1913;
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
			TEXT("Unlit tree can be felled"),
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
	if (!TestNotNull(
		TEXT("Felled trunk and leaves share a detached carrier"),
		Carrier))
	{
		return false;
	}
	const FFragmentAggregateSourceState* CutWoodLayer =
		Carrier->AggregateSources.FindByPredicate(
			[](const FFragmentAggregateSourceState& Layer)
			{
				return !Layer.bOwnsLogicalSource
					&& Layer.MaterialId == TEXT("wood")
					&& Layer.SourceMask.SolidMask.Contains(1);
			});
	if (!TestNotNull(
		TEXT("The other trunk depth layer is cut instead of reattached whole"),
		CutWoodLayer))
	{
		return false;
	}
	const FGuid CutWoodLayerId = CutWoodLayer->SourceId;
	const int32 CutWoodCellIndex =
		CutWoodLayer->SourceMask.SolidMask.IndexOfByKey(1);
	const int32 CutWoodX =
		CutWoodCellIndex % CutWoodLayer->SourceMask.Width;
	const int32 CutWoodY =
		CutWoodCellIndex / CutWoodLayer->SourceMask.Width;
	const FVector CutWoodWorldLocation =
		(CutWoodLayer->LocalTransform * Carrier->GetActorTransform())
		.TransformPosition(FVector(
			(static_cast<float>(CutWoodX) + 0.5f
				- CutWoodLayer->SourceMask.Width * 0.5f)
				* CutWoodLayer->SourceMask.CellSize,
			0.0f,
			(static_cast<float>(CutWoodY) + 0.5f
				- CutWoodLayer->SourceMask.Height * 0.5f)
				* CutWoodLayer->SourceMask.CellSize));
	AActor* FlameAvatar = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Flame test avatar spawns"), FlameAvatar))
	{
		return false;
	}
	USceneComponent* FlameRoot = NewObject<USceneComponent>(FlameAvatar);
	FlameAvatar->SetRootComponent(FlameRoot);
	FlameRoot->RegisterComponent();
	TestEqual(
		TEXT("Detached root preserves wood material identity"),
		Carrier->RootReactionState.MaterialId,
		FName(TEXT("wood")));
	TestTrue(
		TEXT("Detached root exposes a valid reactive voxel mask"),
		Carrier->RootReactionState.IsValid());
	const FBox CarrierBounds = Carrier->GetReactiveWorldBounds();
	TestTrue(
		TEXT("Detached carrier exposes deterministic reactive bounds"),
		CarrierBounds.IsValid != 0);
	const FFragmentSourceMask& RootMask =
		Carrier->RootReactionState.SourceMask;
	const FVector RootHalfExtent(
		RootMask.Width * RootMask.CellSize * 0.5f,
		RootMask.CellSize * 0.5f,
		RootMask.Height * RootMask.CellSize * 0.5f);
	const FBox RootBounds = FBox(-RootHalfExtent, RootHalfExtent).TransformBy(
		Carrier->GetActorTransform().ToMatrixWithScale());
	FlameAvatar->SetActorLocation(FVector(
		RootBounds.Min.X - 240.0f,
		RootBounds.GetCenter().Y,
		RootBounds.Min.Z + Trunk->Mask.CellSize * 1.5f));
	FlameAvatar->SetActorRotation(FRotator::ZeroRotator);
	const int32 RootActivatedTargets = UGA_PlayerFlameJet::ExecuteFlameJet(
		*FlameAvatar,
		600.0f,
		35.0f,
		90.0f,
		TEXT("fire"),
		2201);
	AddInfo(FString::Printf(
		TEXT("Detached root flame cone reported %d activated targets"),
		RootActivatedTargets));
	AMatterFluxMagicProjectile* RootParticle = nullptr;
	for (TActorIterator<AMatterFluxMagicProjectile> It(World); It; ++It)
	{
		if (!It->IsActorBeingDestroyed()
			&& It->GetPresentation().SpellId
			== TEXT("spell.legacy_material_jet"))
		{
			RootParticle = *It;
			break;
		}
	}
	if (!TestNotNull(TEXT("Legacy jet spawns a material particle"), RootParticle))
	{
		return false;
	}
	FHitResult RootHit;
	RootHit.ImpactPoint = RootBounds.GetCenter();
	TestTrue(
		TEXT("Root material particle deposits on impact"),
		RootParticle->ResolveImpactAuthority(RootHit));
	TestFalse(
		TEXT("Particle impact does not synchronously activate the trunk"),
		Carrier->IsRootReacting());
	WorldActor->Tick(0.06f);
	const bool bRootActivatedByWand = Carrier->IsRootReacting();
	TestTrue(
		TEXT("Normal wand material activates detached trunk input"),
		bRootActivatedByWand);
	FTransform LeafWorldTransform;
	if (!TestTrue(
		TEXT("Detached leaf retains its carrier-relative transform"),
		Carrier->GetAggregateSourceWorldTransform(
			Leaf->SourceId,
			LeafWorldTransform)))
	{
		return false;
	}
	FlameAvatar->SetActorLocation(
		LeafWorldTransform.GetLocation() - FVector(240.0f, 0.0f, 0.0f));
	const int32 LeafActivatedTargets = UGA_PlayerFlameJet::ExecuteFlameJet(
		*FlameAvatar,
		600.0f,
		35.0f,
		120.0f,
		TEXT("fire"),
		2202);
	AddInfo(FString::Printf(
		TEXT("Detached leaf flame cone reported %d activated targets"),
		LeafActivatedTargets));
	AMatterFluxMagicProjectile* LeafParticle = nullptr;
	for (TActorIterator<AMatterFluxMagicProjectile> It(World); It; ++It)
	{
		if (!It->IsActorBeingDestroyed()
			&& It->GetPresentation().SpellId
			== TEXT("spell.legacy_material_jet"))
		{
			LeafParticle = *It;
			break;
		}
	}
	if (!TestNotNull(TEXT("Second jet spawns a material particle"), LeafParticle))
	{
		return false;
	}
	FHitResult LeafHit;
	LeafHit.ImpactPoint = LeafWorldTransform.GetLocation();
	TestTrue(
		TEXT("Leaf material particle deposits on impact"),
		LeafParticle->ResolveImpactAuthority(LeafHit));
	WorldActor->Tick(0.06f);
	const bool bLeafActivatedByWand =
		Carrier->IsAnyAggregateMaterialReacting(TEXT("leaf"));
	TestTrue(
		TEXT("A detached leaf part can be activated after cutting"),
		bLeafActivatedByWand);
	TestTrue(
		TEXT("A material particle is deposited on the cut wood layer"),
		WorldActor->SetSimulatedMaterialAtWorldLocation(
			CutWoodWorldLocation,
			TEXT("fire")));
	WorldActor->Tick(0.06f);
	TestTrue(
		TEXT("Wood cut from a secondary trunk layer reacts independently"),
		Carrier->IsAggregateSourceReacting(CutWoodLayerId));
	TestTrue(
		TEXT("The cut wood layer owns a local deterministic reaction runtime"),
		Carrier->IsAggregateSourceReacting(CutWoodLayerId));

	for (int32 Step = 0; Step < 60; ++Step)
	{
		Carrier->Tick(0.1f);
		WorldActor->Tick(0.1f);
	}
	TestTrue(
		TEXT("Detached trunk reaction advances into output"),
		Carrier->GetRootReactionOutputCellCount() > 0);
	TestTrue(
		TEXT("Detached leaf reaction advances into output"),
		WorldActor->GetLogicalReactionOutputCellCount(TEXT("leaf")) > 0);
	FFragmentAggregateSourceState BurnedCutWoodLayer;
	TestTrue(
		TEXT("The cut wood layer remains addressable after active"),
		Carrier->GetAggregateSourceState(
			CutWoodLayerId,
			BurnedCutWoodLayer));
	TestTrue(
		TEXT("Wood separated from the tree advances into output"),
		BurnedCutWoodLayer.OutputMask.SolidMask.Contains(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxDecorationReactionIntegrationTest,
	"MatterFlux.Reaction.DecorationMaskBurnsThroughLuaRule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxDecorationReactionIntegrationTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DSourceActor* Source =
		World ? World->SpawnActor<AFragment2DSourceActor>() : nullptr;
	if (!TestNotNull(TEXT("Input source actor spawns"), Source))
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
			FGuid::NewDeterministicGuid(TEXT("ReactionActorTest"), 1),
			FLinearColor(0.38f, 0.18f, 0.05f),
			TEXT("wood")));
	TestEqual(TEXT("Source retains Lua material identity"),
		Source->SourceMaterialId,
		FName(TEXT("wood")));

	const FVector LocalStimulusPoint(-5.0f, 0.0f, -15.0f);
	TestTrue(TEXT("Configured fire material ignites wood"),
		Source->ApplyMaterialStimulusAtWorldLocation(
			Source->GetActorTransform().TransformPosition(
				LocalStimulusPoint),
			TEXT("fire"),
			404));

	for (int32 Step = 0; Step < 80; ++Step)
	{
		Source->Tick(0.1f);
	}

	TestTrue(TEXT("Wood input mask is consumed"),
		Source->GetRemainingInputCellCount() < 16);
	TestTrue(TEXT("Burned wood leaves solid output"),
		Source->GetOutputCellCount() > 0);
	TestTrue(TEXT("Active emitted visible smoke particles"),
		Source->GetTotalMaterialEmissionCount() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxForestPlantReactionTest,
	"MatterFlux.Reaction.LeavesGrassAndFlowersUseConfiguredReactions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxForestPlantReactionTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Reaction test world exists"), World))
	{
		return false;
	}
	static const FName InputMaterials[] =
	{
		TEXT("leaf"),
		TEXT("grass"),
		TEXT("grassland"),
		TEXT("flower_pink"),
		TEXT("flower_gold"),
		TEXT("flower_blue")
	};
	for (int32 InputIndex = 0;
		InputIndex < UE_ARRAY_COUNT(InputMaterials);
		++InputIndex)
	{
		AFragment2DSourceActor* Source =
			World->SpawnActor<AFragment2DSourceActor>();
		if (!TestNotNull(
			*FString::Printf(
				TEXT("%s source spawns"),
				*InputMaterials[InputIndex].ToString()),
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
				*InputMaterials[InputIndex].ToString()),
			Source->InitializeFromProceduralMask(
				Mask,
				FGuid::NewDeterministicGuid(
					TEXT("ForestPlantReaction"),
					InputIndex + 1),
				FLinearColor::Green,
				InputMaterials[InputIndex]));
		TestTrue(
			*FString::Printf(
				TEXT("%s reacts with fire"),
				*InputMaterials[InputIndex].ToString()),
			Source->ApplyMaterialStimulusAtWorldLocation(
				Source->GetActorLocation(),
				TEXT("fire"),
				500 + InputIndex));
		for (int32 Step = 0; Step < 30; ++Step)
		{
			Source->Tick(0.1f);
		}
		TestTrue(
			*FString::Printf(
				TEXT("%s leaves solid ash"),
				*InputMaterials[InputIndex].ToString()),
			Source->GetOutputCellCount() > 0);
	}
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxReactionStateRoundTripTest,
	"MatterFlux.Reaction.StateSnapshotResumesDeterministically",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxReactionStateRoundTripTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = 5;
	Mask.Height = 4;
	Mask.SolidMask.Init(1, Mask.Width * Mask.Height);

	FMatterFluxReactionDefinition Rule;
	Rule.Id = TEXT("snapshot_fire");
	Rule.InputA = TEXT("wood");
	Rule.InputB = TEXT("fire");
	Rule.EmissionMaterial = TEXT("smoke");
	Rule.OutputA = TEXT("charcoal");
	Rule.ChancePermille = 1000;
	Rule.PropagationChancePermille = 730;
	Rule.EmissionChancePermille = 610;
	Rule.DurationSteps = 7;

	MatterFlux::Reaction::FMaskReaction Authority;
	TestTrue(TEXT("Authority reaction initializes"),
		Authority.Initialize(Mask, Rule, 4242));
	TestTrue(TEXT("Authority reaction ignites"),
		Authority .Activate(FIntPoint(2, 1), TEXT("fire")));
	for (int32 Step = 0; Step < 4; ++Step)
	{
		Authority.Step();
	}

	MatterFlux::Reaction::FStateSnapshot Snapshot;
	TestTrue(TEXT("Reaction exports its complete deterministic state"),
		Authority.CaptureState(Snapshot));
	MatterFlux::Reaction::FMaskReaction Restored;
	FString Error;
	TestTrue(TEXT("Reaction restores without replaying missed steps"),
		Restored.RestoreState(Snapshot, Rule, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
	}
	TestTrue(TEXT("Restored input is exact"),
		Restored.GetInputMask() == Authority.GetInputMask());
	TestTrue(TEXT("Restored output is exact"),
		Restored.GetOutputMask() == Authority.GetOutputMask());
	TestTrue(TEXT("Restored burn durations are exact"),
		Restored.GetActiveMask() == Authority.GetActiveMask());

	for (int32 Step = 0; Step < 12; ++Step)
	{
		const MatterFlux::Reaction::FStepStats AuthorityStats =
			Authority.Step();
		const MatterFlux::Reaction::FStepStats RestoredStats =
			Restored.Step();
		TestEqual(TEXT("Resumed ignition count stays deterministic"),
			RestoredStats.ActivatedCells,
			AuthorityStats.ActivatedCells);
		TestEqual(TEXT("Resumed consumption count stays deterministic"),
			RestoredStats.ConsumedInputCells,
			AuthorityStats.ConsumedInputCells);
		TestTrue(TEXT("Resumed smoke cells stay deterministic"),
			RestoredStats.MaterialEmissionCells
				== AuthorityStats.MaterialEmissionCells);
	}
	return true;
}
