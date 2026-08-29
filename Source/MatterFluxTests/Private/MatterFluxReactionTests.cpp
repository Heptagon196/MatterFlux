#include "Material/MatterFluxLocalMaterialReaction.h"
#include "Material/MatterFluxMaterialContactGeometry.h"
#include "Fragment/Fragment2DActor.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/Fragment2DSourceStreamingState.h"
#include "Fragment/FragmentGeometry.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Game/MatterFluxPlayableLevel.h"
#include "Game/MatterFluxFragmentSourceReplication.h"
#include "GAS/GA_CastWand.h"
#include "GAS/GA_PlayerFlameJet.h"
#include "IMatterFluxScriptRuntime.h"
#include "Magic/MatterFluxMagicProjectile.h"
#include "Magic/MatterFluxWandProgram.h"
#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "ProceduralMeshComponent.h"
#include "Rendering/MatterFluxSmokeVisualPool.h"
#include "Save/MatterFluxSaveGame.h"
#include "Tests/AutomationEditorCommon.h"
#include "Algo/Count.h"

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
		Fire.RadialContactRadius, CellSize * 0.75f);
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
	FMatterFluxSourceStreamingMaskStorageTest,
	"MatterFlux.Volume.StreamingStateStoresOneCanonicalOccupancyMask",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxSourceStreamingMaskStorageTest::RunTest(
	const FString& Parameters)
{
	constexpr int32 Width = 8;
	constexpr int32 Height = 8;
	constexpr int32 CellCount = Width * Height;
	FFragment2DSourceStreamingState State;
	TArray<uint8> Occupancy;
	Occupancy.Init(1, CellCount);
	State.SetRuntimeMask(Occupancy);
	TestTrue(
		TEXT("Effective runtime mask is the supplied occupancy truth"),
		State.GetRuntimeMask() == Occupancy);
	TestEqual(
		TEXT("Streaming state stores occupancy exactly once"),
		State.GetStoredMaskValueCount(),
		CellCount);
	State.VolumeCellStates.Add({FIntVector(3, 4, 0), TEXT("charcoal"), 42000});
	TestEqual(
		TEXT("Sparse material facts add no second dense mask"),
		State.GetStoredMaskValueCount(),
		CellCount + static_cast<int32>(sizeof(FFragment2DMaterialVolumeCellState)));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSourceReactionCutSynchronizationTest,
	"MatterFlux.Reaction.CutAtomicallySynchronizesOccupancyAndVolumeRevision",
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
	FFragment2DSourceStreamingState BeforeState;
	FString Error;
	if (!TestTrue(TEXT("Pre-cut Volume state captures"),
		Source->CaptureStreamingState(BeforeState, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("Pre-cut state has no sparse material overrides"),
		BeforeState.VolumeCellStates.IsEmpty());

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

	FFragment2DSourceStreamingState AfterState;
	if (!TestTrue(TEXT("Post-cut Volume state captures"),
		Source->CaptureStreamingState(AfterState, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Cut removes the occupied material element"),
		Source->GetRemainingInputCellCount(), 0);
	TestEqual(TEXT("Replicated occupancy changes in the same transaction"),
		AfterState.GetRuntimeMask()[0], static_cast<uint8>(0));
	TestEqual(TEXT("The source revision advances exactly once"),
		AfterState.Revision, BeforeState.Revision + 1);
	TestEqual(TEXT("The topology revision advances exactly once"),
		AfterState.VolumeTopologyRevision,
		BeforeState.VolumeTopologyRevision + 1);
	TestTrue(TEXT("Cut synchronization preserves sparse material facts"),
		AfterState.VolumeCellStates.IsEmpty());
	FMatterFluxReplicatedFragmentSourceStateList ReplicatedStates;
	const FMatterFluxFragmentSourceStateBatchUpdate Update{
		Source->SourceId, &AfterState };
	TestEqual(TEXT("Post-cut state commits atomically to FastArray replication"),
		ReplicatedStates.UpsertAuthorityBatch({ Update }, 1, 1),
		EMatterFluxFragmentSourceStateUpsertResult::Committed);
	if (!TestEqual(TEXT("One post-cut replication item exists"),
		ReplicatedStates.Items.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("Single-cell occupancy remains one packed byte"),
		ReplicatedStates.Items[0].PackedRuntimeMask.Num(), 1);
	TestEqual(TEXT("The removed cell is clear in the packed occupancy"),
		ReplicatedStates.Items[0].PackedRuntimeMask[0], static_cast<uint8>(0));
	TestEqual(TEXT("The source delta carries occupancy only"),
		ReplicatedStates.GetAuthorityPayloadByteCount(), 1);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSourceReactionReplicationBudgetTest,
	"MatterFlux.Reaction.SourceReplicationPacksOccupancyAndSparseVolumeFields",
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
	FFragment2DSourceStreamingState InitialState;
	FString Error;
	if (!TestTrue(TEXT("Maximum source Volume state captures"),
		Source->CaptureStreamingState(InitialState, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("The pristine Volume has no sparse field state"),
		InitialState.VolumeCellStates.IsEmpty());
	TestTrue(TEXT("One field-only edit commits without topology mutation"),
		Source->CommitMaterialVolumeCellEnergy(
			FIntVector(128, 128, 0),
			InitialState.VolumeEnvironmentEnergy,
			InitialState.VolumeEnvironmentEnergy,
			42000));

	FFragment2DSourceStreamingState ChangedState;
	if (!TestTrue(TEXT("Edited Volume state captures"),
		Source->CaptureStreamingState(ChangedState, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Exactly one sparse Volume cell is encoded"),
		ChangedState.VolumeCellStates.Num(), 1);
	TestEqual(TEXT("A field edit remains one sparse Volume fact"),
		ChangedState.VolumeCellStates.Num(), 1);

	FMatterFluxReplicatedFragmentSourceStateList ReplicatedStates;
	const FMatterFluxFragmentSourceStateBatchUpdate Update{
		Source->SourceId, &ChangedState };
	const int32 PackedOccupancyBytes = FMath::DivideAndRoundUp(
		Mask.Width * Mask.Height, 8);
	const int32 MaximumPayloadBytes = PackedOccupancyBytes
		+ sizeof(FMatterFluxReplicatedVolumeCellState);
	TestEqual(TEXT("Sparse Volume state commits atomically to replication"),
		ReplicatedStates.UpsertAuthorityBatch(
			{ Update }, 1, MaximumPayloadBytes),
		EMatterFluxFragmentSourceStateUpsertResult::Committed);
	if (!TestEqual(TEXT("One Source replication item is stored"),
		ReplicatedStates.Items.Num(), 1))
	{
		return false;
	}
	const FMatterFluxReplicatedFragmentSourceState& Item =
		ReplicatedStates.Items[0];
	TestEqual(TEXT("Occupancy is bit packed once"),
		Item.PackedRuntimeMask.Num(), PackedOccupancyBytes);
	TestEqual(TEXT("One sparse energy cell is replicated"),
		Item.VolumeCellStates.Num(), 1);
	TestEqual(TEXT("The sparse energy value survives packing"),
		Item.VolumeCellStates[0].Energy, static_cast<uint16>(42000));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLocalParticleAvoidsObjectReactionStateTest,
	"MatterFlux.Material.Interactions.LocalParticleDoesNotCreateObjectReactionState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLocalParticleAvoidsObjectReactionStateTest::RunTest(
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
		WorldActor->GetHotSourceCount(),
		0);

	WorldActor->Tick(0.06f);
	TestTrue(
		TEXT("The hot-source query is derived from locally heated Volume cells"),
		WorldActor->GetHotSourceCount() > 0);

	FMatterFluxWorldSaveState WorldState;
	FString Error;
	if (!TestTrue(
		TEXT("The local contact can be captured without an object reaction runtime"),
		WorldActor->CaptureSaveState(WorldState, Error)))
	{
		AddError(Error);
		return false;
	}
	TestFalse(
		TEXT("The local material element creates no ground ReactionState"),
		WorldState.bHasGroundReactionState);
	TestFalse(
		TEXT("The local material element creates no source ReactionState"),
		WorldState.FragmentSources.ContainsByPredicate(
			[](const FMatterFluxSavedFragmentSourceState& State)
			{
				return State.bHasReactionState;
			}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLocalReactionSaveHasNoObjectStateTest,
	"MatterFlux.Save.LocalReactionContainsNoObjectReactionState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLocalReactionSaveHasNoObjectStateTest::RunTest(
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
	TestTrue(TEXT("A stimulus is accepted as a local material contact"),
		WorldActor->ApplyMaterialStimulusToFirstGeneratedTree(991));
	TestEqual(TEXT("The public ignition entry point creates no object reaction"),
		WorldActor->GetHotSourceCount(), 0);
	WorldActor->Tick(0.06f);

	FMatterFluxWorldSaveState WorldState;
	FString Error;
	if (!TestTrue(
		TEXT("The local-reaction world can be captured immediately"),
		WorldActor->CaptureSaveState(WorldState, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("The contact writes a persistent Volume energy/material fact"),
		WorldState.FragmentSources.ContainsByPredicate(
			[](const FMatterFluxSavedFragmentSourceState& State)
			{
				return State.VolumeFieldRevision > 0
					&& !State.VolumeCellStates.IsEmpty();
			}));

	UMatterFluxSaveGame* Save = NewObject<UMatterFluxSaveGame>();
	Save->InitializeNew(1337);
	Save->WorldState = MoveTemp(WorldState);
	const bool bValid = Save->ValidateAndMigrate(Error);
	if (!bValid)
	{
		AddError(Error);
	}
	TestTrue(TEXT("The V6 local-reaction save is valid"), bValid);
	TestFalse(TEXT("V6 save does not carry a ground ReactionState"),
		Save->WorldState.bHasGroundReactionState);
	TestFalse(TEXT("V6 save does not carry source ReactionState"),
		Save->WorldState.FragmentSources.ContainsByPredicate(
			[](const FMatterFluxSavedFragmentSourceState& State)
			{
				return State.bHasReactionState;
			}));
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
	FGuid AggregateId;
	FGuid RootSourceId;
	FBox AggregateBounds;
	FTransform RootWorldTransform;
	if (!TestTrue(TEXT("A generated tree can be selected for material impact"),
		WorldActor->FindNearestTreeAggregateForVisualInspection(
			FVector::ZeroVector,
			AggregateId,
			RootSourceId,
			AggregateBounds,
			RootWorldTransform)))
	{
		return false;
	}
	// Reproduce the gameplay handoff: the material sweep first materializes only
	// the source it actually touched.  The fire impact itself must promote the
	// bounded aggregate into the local-reaction graph; pre-materializing the
	// entire tree here hid the projectile-only failure mode.
	TArray<AFragment2DSourceActor*> ContactSources;
	WorldActor->GatherFragmentSourcesInBounds(
		FBox::BuildAABB(
			RootWorldTransform.GetLocation(),
			FVector(24.0f)),
		ContactSources);
	AFragment2DSourceActor* RootSource = nullptr;
	for (AFragment2DSourceActor* Candidate : ContactSources)
	{
		if (IsValid(Candidate) && Candidate->GetSourceId() == RootSourceId)
		{
			RootSource = Candidate;
			break;
		}
	}
	if (!TestNotNull(TEXT("The impacted tree root is materialized"), RootSource))
	{
		return false;
	}
	const FVector LocalImpactPoint(
		0.0f,
		0.0f,
		(-static_cast<float>(RootSource->GetMaskHeight()) * 0.5f + 0.5f)
			* RootSource->GetCellSize());
	const FVector WorldImpactPoint =
		RootSource->GetActorTransform().TransformPosition(LocalImpactPoint);
	TestTrue(TEXT("A fire projectile impact deposits an ordinary material element"),
		WorldActor->DepositSimulatedMaterialFromImpact(
			WorldImpactPoint,
			TEXT("fire"),
			1,
			RootSource,
			RootSource->GetCellSize()) > 0);
	WorldActor->Tick(0.1f);
	TestTrue(TEXT("The first local step exposes a hot tree source"),
		WorldActor->GetHotSourceCount() > 0);
	TestTrue(TEXT("The first local step publishes visible source flames"),
		WorldActor->GetLogicalMaterialFlameInstanceCount() > 0);
	int32 MaterializedAggregateSources = 0;
	int32 AggregateOccupiedCells = 0;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		if (!It->IsActorBeingDestroyed() && It->AggregateId == AggregateId)
		{
			++MaterializedAggregateSources;
			AggregateOccupiedCells += It->GetRemainingInputCellCount();
		}
	}
	TestTrue(
		TEXT("A direct fire impact materializes every bounded Volume in its tree"),
		MaterializedAggregateSources > 1);
	TestTrue(TEXT("The materialized aggregate contains combustible cells"),
		AggregateOccupiedCells > 0);

	int32 MaximumSmokeParticles = 0;
	int32 MaximumSettledSmokeCells = 0;
	for (int32 Step = 1; Step < 600; ++Step)
	{
		WorldActor->Tick(0.1f);
		MaximumSmokeParticles = FMath::Max(
			MaximumSmokeParticles,
			WorldActor->GetAirborneSimulatedMaterialParticleCount(TEXT("smoke")));
		TArray<MatterFlux::Material::FCellSnapshot> SmokeCells;
		WorldActor->GetSimulatedMaterialCells(TEXT("smoke"), SmokeCells);
		MaximumSettledSmokeCells = FMath::Max(
			MaximumSettledSmokeCells, SmokeCells.Num());
	}
	const auto IsAggregateFullyConsumed = [World, AggregateId]()
	{
		for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
		{
			if (!It->IsActorBeingDestroyed()
				&& It->AggregateId == AggregateId
				&& It->GetOutputCellCount()
					!= It->GetRemainingInputCellCount())
			{
				return false;
			}
		}
		return true;
	};
	int32 ConvergenceSteps = 0;
	while (ConvergenceSteps < 600
		&& (!IsAggregateFullyConsumed()
			|| WorldActor->GetLogicalMaterialFlameInstanceCount() > 0))
	{
		WorldActor->Tick(0.1f);
		++ConvergenceSteps;
	}
	AddInfo(FString::Printf(
		TEXT("Tree combustion converged after %d bounded follow-up steps"),
		ConvergenceSteps));

	FMatterFluxWorldSaveState LocalState;
	FString CaptureError;
	if (!TestTrue(TEXT("Local Volume reaction state captures"),
		WorldActor->CaptureSaveState(LocalState, CaptureError)))
	{
		AddError(CaptureError);
		return false;
	}
	TestTrue(TEXT("The independent local-reaction fixed step advances"),
		LocalState.LocalMaterialReactionStep > 0);
	int32 BurnedWoodCells = 0;
	int32 BurnedLeafCells = 0;
	for (const FMatterFluxSavedFragmentSourceState& SourceState
		: LocalState.FragmentSources)
	{
		for (const FMatterFluxSavedVolumeCellState& Cell
			: SourceState.VolumeCellStates)
		{
			BurnedWoodCells += Cell.MaterialId == TEXT("charcoal") ? 1 : 0;
			BurnedLeafCells += Cell.MaterialId == TEXT("ash") ? 1 : 0;
		}
	}
	TestTrue(TEXT("The trunk becomes charcoal"), BurnedWoodCells > 0);
	TestTrue(TEXT("Fire crosses source boundaries into the canopy"),
		BurnedLeafCells > 0);
	TestTrue(TEXT("The fire consumes every material cell in the tree"),
		IsAggregateFullyConsumed());
	TestEqual(TEXT("Combustion visuals extinguish after the tree is consumed"),
		WorldActor->GetLogicalMaterialFlameInstanceCount(), 0);
	TestTrue(TEXT("The active tree generates smoke particles"),
		MaximumSmokeParticles > 0 || MaximumSettledSmokeCells > 0);
	TestFalse(TEXT("No source stores object-level ReactionState"),
		LocalState.FragmentSources.ContainsByPredicate(
			[](const FMatterFluxSavedFragmentSourceState& State)
			{
				return State.bHasReactionState;
			}));
	TestEqual(
		TEXT("Burned proxy cells never overlap a resurrected pristine tree"),
		WorldActor->GetLogicalMaterialProjectionOverlapCellCount(),
		0);
	TestEqual(
		TEXT("Burned tree material is part of the unified voxel object"),
		WorldActor->GetStandaloneTreeMaterialOverrideProjectionCount(),
		0);
	TestFalse(TEXT("Ground also stores no object-level ReactionState"),
		LocalState.bHasGroundReactionState);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPlayableForestReactionStressTest,
	"MatterFlux.Reaction.PlayableForestMultipleTreesBurnAndExtinguish",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxPlayableForestReactionStressTest::RunTest(
	const FString& Parameters)
{
	constexpr int32 LevelSeed = 1337;
	constexpr int32 TargetTreeCount = 6;
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
			LevelSeed, Layout, Registry.Get())))
	{
		return false;
	}
	WorldActor->Regenerate(LevelSeed);

	TArray<const MatterFlux::PlayableLevel::FLevelFragmentSource*> TreeRoots;
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
		: Layout.FragmentSources)
	{
		if (Source.Name == TEXT("TreeTrunk")
			&& Source.MaterialId == TEXT("wood")
			&& Source.bAggregateRoot
			&& Source.AggregateId.IsValid()
			&& Source.Mask.IsValid())
		{
			TreeRoots.Add(&Source);
		}
	}
	if (!TestTrue(TEXT("The generated forest has enough distinct trees"),
		TreeRoots.Num() >= TargetTreeCount))
	{
		return false;
	}

	TSet<FGuid> TargetAggregateIds;
	TMap<FGuid, int32> ExpectedMemberCounts;
	for (int32 TreeIndex = 0; TreeIndex < TargetTreeCount; ++TreeIndex)
	{
		const MatterFlux::PlayableLevel::FLevelFragmentSource& Root =
			*TreeRoots[TreeIndex];
		TargetAggregateIds.Add(Root.AggregateId);
		int32 ExpectedMembers = 0;
		for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
			: Layout.FragmentSources)
		{
			ExpectedMembers += Source.AggregateId == Root.AggregateId ? 1 : 0;
		}
		ExpectedMemberCounts.Add(Root.AggregateId, ExpectedMembers);

		const FTransform RootWorldTransform =
			Root.Transform * WorldActor->GetActorTransform();
		TArray<AFragment2DSourceActor*> ContactSources;
		WorldActor->GatherFragmentSourcesInBounds(
			FBox::BuildAABB(
				RootWorldTransform.GetLocation(),
				FVector(Root.Mask.CellSize * 1.5f)),
			ContactSources);
		AFragment2DSourceActor* RootActor = nullptr;
		for (AFragment2DSourceActor* Candidate : ContactSources)
		{
			if (IsValid(Candidate) && Candidate->GetSourceId() == Root.SourceId)
			{
				RootActor = Candidate;
				break;
			}
		}
		if (!TestNotNull(
			FString::Printf(TEXT("Tree %d root materializes"), TreeIndex),
			RootActor))
		{
			return false;
		}

		for (int32 ImpactIndex = 0; ImpactIndex < 3; ++ImpactIndex)
		{
			const int32 SolidX = ((TreeIndex + ImpactIndex) & 1) == 0 ? 1 : 2;
			const int32 SolidY = ImpactIndex;
			const FVector LocalImpactPoint(
				(static_cast<float>(SolidX) + 0.5f
					- static_cast<float>(RootActor->GetMaskWidth()) * 0.5f)
					* RootActor->GetCellSize(),
				0.0f,
				(static_cast<float>(SolidY) + 0.5f
					- static_cast<float>(RootActor->GetMaskHeight()) * 0.5f)
					* RootActor->GetCellSize());
			const FVector WorldImpactPoint =
				RootActor->GetActorTransform().TransformPosition(LocalImpactPoint);
			TestTrue(
				FString::Printf(
					TEXT("Tree %d impact %d deposits fire"),
					TreeIndex,
					ImpactIndex),
				WorldActor->DepositSimulatedMaterialFromImpact(
					WorldImpactPoint,
					TEXT("fire"),
					1,
					RootActor,
					RootActor->GetCellSize()) > 0);
		}
	}

	auto GetForestProgress = [World, &TargetAggregateIds, &ExpectedMemberCounts](
		TMap<FGuid, int32>& OutRemainingCells,
		TMap<FGuid, int32>& OutMemberCounts)
	{
		OutRemainingCells.Reset();
		OutMemberCounts.Reset();
		for (const FGuid& AggregateId : TargetAggregateIds)
		{
			OutRemainingCells.Add(AggregateId, 0);
			OutMemberCounts.Add(AggregateId, 0);
		}
		for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
		{
			if (It->IsActorBeingDestroyed()
				|| !TargetAggregateIds.Contains(It->AggregateId))
			{
				continue;
			}
			OutMemberCounts.FindChecked(It->AggregateId) += 1;
			OutRemainingCells.FindChecked(It->AggregateId) +=
				It->GetRemainingInputCellCount() - It->GetOutputCellCount();
		}
		for (const FGuid& AggregateId : TargetAggregateIds)
		{
			if (OutMemberCounts.FindChecked(AggregateId)
				!= ExpectedMemberCounts.FindChecked(AggregateId)
				|| OutRemainingCells.FindChecked(AggregateId) != 0)
			{
				return false;
			}
		}
		return true;
	};

	TMap<FGuid, int32> RemainingCells;
	TMap<FGuid, int32> MemberCounts;
	int32 ConvergenceSteps = 0;
	while (ConvergenceSteps < 2400)
	{
		WorldActor->Tick(0.1f);
		++ConvergenceSteps;
		if (GetForestProgress(RemainingCells, MemberCounts)
			&& WorldActor->GetLogicalMaterialFlameInstanceCount() == 0)
		{
			break;
		}
	}
	AddInfo(FString::Printf(
		TEXT("Six-tree forest combustion converged after %d fixed steps"),
		ConvergenceSteps));
	for (const FGuid& AggregateId : TargetAggregateIds)
	{
		const FString ShortId = AggregateId.ToString(EGuidFormats::Short);
		TestEqual(
			FString::Printf(TEXT("Tree %s materializes every member"), *ShortId),
			MemberCounts.FindRef(AggregateId),
			ExpectedMemberCounts.FindChecked(AggregateId));
		TestEqual(
			FString::Printf(TEXT("Tree %s has no unburned cells"), *ShortId),
			RemainingCells.FindRef(AggregateId),
			0);
	}
	TestEqual(TEXT("The stressed forest leaves no logical flames"),
		WorldActor->GetLogicalMaterialFlameInstanceCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxRealFlameSpellTreeReactionTest,
	"MatterFlux.Reaction.RealFlameSpellConsumesTreeFromCanopyWithoutMaterialFlicker",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxRealFlameSpellTreeReactionTest::RunTest(
	const FString& Parameters)
{
	constexpr int32 LevelSeed = 1337;
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(TEXT("Default content pack loads"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	const FMatterFluxWandDefinition* FlameWand = Registry.IsValid()
		? Registry->Wands.Find(TEXT("wand.flame"))
		: nullptr;
	if (!TestNotNull(TEXT("Real flame wand content exists"), FlameWand))
	{
		return false;
	}

	MatterFlux::PlayableLevel::FLevelLayout Layout;
	if (!TestTrue(TEXT("Reference forest layout builds"),
		MatterFlux::PlayableLevel::BuildLevelLayout(
			LevelSeed, Layout, Registry.Get())))
	{
		return false;
	}
	const MatterFlux::PlayableLevel::FLevelFragmentSource* Root =
		Layout.FragmentSources.FindByPredicate(
			[](const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
			{
				return Source.Name == TEXT("TreeTrunk")
					&& Source.bAggregateRoot
					&& Source.AggregateId.IsValid();
			});
	if (!TestNotNull(TEXT("Generated tree root exists"), Root))
	{
		return false;
	}
	const MatterFlux::PlayableLevel::FLevelFragmentSource* TargetLeaf =
		Layout.FragmentSources.FindByPredicate(
			[Root](const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
			{
				return Source.AggregateId == Root->AggregateId
					&& Source.MaterialId == TEXT("leaf")
					&& Source.Mask.IsValid()
					&& Source.Mask.SolidMask.Contains(1);
			});
	if (!TestNotNull(TEXT("Generated tree canopy exists"), TargetLeaf))
	{
		return false;
	}

	const int32 TargetCellIndex = TargetLeaf->Mask.SolidMask.IndexOfByKey(1);
	const int32 TargetX = TargetCellIndex % TargetLeaf->Mask.Width;
	const int32 TargetY = TargetCellIndex / TargetLeaf->Mask.Width;
	const FVector LocalTarget(
		(static_cast<float>(TargetX) + 0.5f
			- static_cast<float>(TargetLeaf->Mask.Width) * 0.5f)
			* TargetLeaf->Mask.CellSize,
		0.0f,
		(static_cast<float>(TargetY) + 0.5f
			- static_cast<float>(TargetLeaf->Mask.Height) * 0.5f)
			* TargetLeaf->Mask.CellSize);
	const FVector Target = TargetLeaf->Transform.TransformPosition(LocalTarget);
	FVector AimDirection = TargetLeaf->Transform.TransformVectorNoScale(
		FVector::YAxisVector);
	AimDirection.Z = 0.0f;
	AimDirection = AimDirection.GetSafeNormal();
	if (AimDirection.IsNearlyZero())
	{
		AimDirection = FVector::ForwardVector;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor = World
		? World->SpawnActor<AMatterFluxPlayableWorldActor>() : nullptr;
	AActor* Avatar = World ? World->SpawnActor<AActor>() : nullptr;
	if (!TestNotNull(TEXT("Playable world spawns"), WorldActor)
		|| !TestNotNull(TEXT("Spell caster spawns"), Avatar))
	{
		return false;
	}
	USphereComponent* AvatarRoot = NewObject<USphereComponent>(Avatar);
	AvatarRoot->SetSphereRadius(35.0f);
	AvatarRoot->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Avatar->SetRootComponent(AvatarRoot);
	Avatar->AddInstanceComponent(AvatarRoot);
	AvatarRoot->RegisterComponent();
	WorldActor->Regenerate(LevelSeed);
	Avatar->SetActorLocation(
		Target - AimDirection * 300.0f - FVector::UpVector * 25.0f);
	Avatar->SetActorRotation(AimDirection.Rotation());

	FMatterFluxWandProgramState WandState;
	WandState.Mana = FlameWand->ManaMax;
	FMatterFluxWandCastPlan CastPlan;
	if (!TestTrue(TEXT("Real flame wand compiles"),
		FMatterFluxWandProgram::Evaluate(
			*Registry,
			FlameWand->Id,
			FlameWand->StarterDeck,
			WandState,
			741,
			CastPlan,
			Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestTrue(TEXT("Real flame spell launches at the canopy"),
		UGA_CastWand::SpawnCastPlan(
			*Avatar, CastPlan, 741, AimDirection)))
	{
		return false;
	}
	TArray<AMatterFluxMagicProjectile*> Projectiles;
	for (TActorIterator<AMatterFluxMagicProjectile> It(World); It; ++It)
	{
		Projectiles.Add(*It);
		if (!It->HasActorBegunPlay())
		{
			It->DispatchBeginPlay();
		}
	}
	TestTrue(TEXT("The cast creates real flame material particles"),
		WorldActor->GetAirborneSimulatedMaterialParticleCount(TEXT("fire")) > 0);

	bool bCanopyIgnited = false;
	for (int32 Frame = 0; Frame < 90; ++Frame)
	{
		WorldActor->Tick(1.0f / 60.0f);
		for (AMatterFluxMagicProjectile* Projectile : Projectiles)
		{
			if (IsValid(Projectile) && !Projectile->IsActorBeingDestroyed())
			{
				Projectile->Tick(1.0f / 60.0f);
			}
		}
		for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
		{
			bCanopyIgnited |= It->AggregateId == Root->AggregateId
				&& (It->GetActiveCellCount() > 0
					|| It->GetOutputCellCount() > 0);
		}
	}
	if (!TestTrue(TEXT("One real canopy hit deterministically ignites the tree"),
		bCanopyIgnited))
	{
		return false;
	}

	TMap<FGuid, FName> ExpectedProductBySource;
	TMap<FGuid, int32> ExpectedCellCountBySource;
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
		: Layout.FragmentSources)
	{
		if (Source.AggregateId != Root->AggregateId)
		{
			continue;
		}
		ExpectedProductBySource.Add(
			Source.SourceId,
			Source.MaterialId == TEXT("wood")
				? FName(TEXT("charcoal")) : FName(TEXT("ash")));
		ExpectedCellCountBySource.Add(
			Source.SourceId,
			Algo::Count(Source.Mask.SolidMask, static_cast<uint8>(1)));
	}

	auto IsTreeConsumed = [World, Root]()
	{
		int32 SeenMembers = 0;
		for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
		{
			if (It->IsActorBeingDestroyed()
				|| It->AggregateId != Root->AggregateId)
			{
				continue;
			}
			++SeenMembers;
			if (It->GetOutputCellCount() != It->GetRemainingInputCellCount())
			{
				return false;
			}
		}
		return SeenMembers > 0;
	};
	int32 ConvergenceSteps = 0;
	while (ConvergenceSteps < 1800
		&& (!IsTreeConsumed()
			|| WorldActor->GetLogicalMaterialFlameInstanceCount() > 0))
	{
		WorldActor->Tick(0.1f);
		++ConvergenceSteps;
	}
	AddInfo(FString::Printf(
		TEXT("Real canopy flame consumed the tree after launch plus %d fixed steps"),
		ConvergenceSteps));
	TestTrue(TEXT("The real spell consumes every trunk and leaf cell"),
		IsTreeConsumed());
	TestEqual(TEXT("Converted tree cells have one visual material owner"),
		WorldActor->GetLogicalMaterialProjectionOverlapCellCount(), 0);
	TestEqual(TEXT("The completed tree fire extinguishes"),
		WorldActor->GetLogicalMaterialFlameInstanceCount(), 0);

	FMatterFluxWorldSaveState Saved;
	if (!TestTrue(TEXT("Consumed real-spell tree state captures"),
		WorldActor->CaptureSaveState(Saved, Error)))
	{
		AddError(Error);
		return false;
	}
	for (const TPair<FGuid, FName>& Expected : ExpectedProductBySource)
	{
		const FMatterFluxSavedFragmentSourceState* SourceState =
			Saved.FragmentSources.FindByPredicate(
				[&Expected](const FMatterFluxSavedFragmentSourceState& State)
				{
					return State.SourceId == Expected.Key;
				});
		if (!TestNotNull(TEXT("Every tree member has authoritative Volume state"),
			SourceState))
		{
			continue;
		}
		const int32 ProductCells = Algo::CountIf(
			SourceState->VolumeCellStates,
			[&Expected](const FMatterFluxSavedVolumeCellState& Cell)
			{
				return Cell.MaterialId == Expected.Value;
			});
		TestEqual(TEXT("Every occupied member cell has its combustion product"),
			ProductCells,
			ExpectedCellCountBySource.FindChecked(Expected.Key));
	}
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
	// Thermal ignition is deterministic and has no event-seeded propagation
	// probability. Seed variation belongs to contact-rule probability tests, not
	// to this end-to-end Volume reachability check.
	constexpr int32 EventSeeds[] = { 1 };
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
		FMatterFluxWorldSaveState BurnedState;
		FString CaptureError;
		if (!TestTrue(TEXT("Burned tree Volume state captures"),
			WorldActor->CaptureSaveState(BurnedState, CaptureError)))
		{
			AddError(CaptureError);
			return false;
		}
		TestFalse(TEXT("A top-canopy burn stores no object ReactionState"),
			BurnedState.FragmentSources.ContainsByPredicate(
				[](const FMatterFluxSavedFragmentSourceState& State)
				{
					return State.bHasReactionState;
				}));

		for (const MatterFlux::PlayableLevel::FLevelFragmentSource* Source
			: TopLeafSources)
		{
			if (!Source)
			{
				return false;
			}
			const FMatterFluxSavedFragmentSourceState* SourceState =
				BurnedState.FragmentSources.FindByPredicate(
					[Source](const FMatterFluxSavedFragmentSourceState& State)
					{
						return State.SourceId == Source->SourceId;
					});
			if (!SourceState)
			{
				bEveryTopLeafBurned = false;
				AddError(FString::Printf(
					TEXT("Fire seed %d lost top leaf Volume source %s"),
					EventSeed,
					*Source->SourceId.ToString(EGuidFormats::Digits)));
				continue;
			}
			int32 RemainingCells = 0;
			FString RemainingCoordinates;
			for (const FMatterFluxSavedVolumeCellState& Cell
				: SourceState->VolumeCellStates)
			{
				if (Cell.MaterialId == TEXT("ash"))
				{
					continue;
				}
				++RemainingCells;
				RemainingCoordinates += FString::Printf(
					TEXT(" (%d,%d,%d):%s"),
					Cell.Cell.X,
					Cell.Cell.Y,
					Cell.Cell.Z,
					*Cell.MaterialId.ToString());
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
	TestTrue(TEXT("Fire converts every top-canopy leaf cell to ash"),
		bEveryTopLeafBurned);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterializedFireKeepsLogicalNeighborsTest,
	"MatterFlux.Reaction.ContactMaterializesBoundedVolumesWithoutReactionState",
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
	if (!TestNotNull(TEXT("Generated tree trunk exists"), Trunk))
	{
		return false;
	}
	TestEqual(TEXT("The untouched forest starts as batched projections"),
		WorldActor->GetGeneratedFragmentSourceCount(), 0);
	if (!TestTrue(TEXT("A fire element contacts the aggregate root"),
		WorldActor->ApplyMaterialStimulusToFirstGeneratedTree(923)))
	{
		return false;
	}
	WorldActor->Tick(0.06f);
	AFragment2DSourceActor* ContactedRoot = nullptr;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		FFragment2DSourceStreamingState CandidateState;
		FString CandidateError;
		if (!It->IsActorBeingDestroyed()
			&& It->bAggregateRoot
			&& It->SourceMaterialId == TEXT("wood")
			&& It->CaptureStreamingState(CandidateState, CandidateError)
			&& !CandidateState.VolumeCellStates.IsEmpty())
		{
			ContactedRoot = *It;
			break;
		}
	}
	if (!TestNotNull(TEXT("The contacted aggregate root materializes"),
		ContactedRoot))
	{
		return false;
	}
	const FGuid ContactedAggregateId = ContactedRoot->AggregateId;
	const FGuid ContactedRootId = ContactedRoot->SourceId;
	TSet<FGuid> AggregateMemberIds;
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
		: Layout.FragmentSources)
	{
		if (Source.AggregateId == ContactedAggregateId)
		{
			AggregateMemberIds.Add(Source.SourceId);
		}
	}
	const int32 AggregateMemberCount = AggregateMemberIds.Num();
	if (!TestTrue(TEXT("The contacted tree has multiple Volume members"),
		AggregateMemberCount > 1))
	{
		return false;
	}
	TSet<FGuid> MaterializedSourceIds;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		if (!It->IsActorBeingDestroyed())
		{
			MaterializedSourceIds.Add(It->SourceId);
		}
	}
	bool bEveryAggregateMemberScheduled = true;
	for (const FGuid& SourceId : AggregateMemberIds)
	{
		bEveryAggregateMemberScheduled &= MaterializedSourceIds.Contains(SourceId);
	}
	TestTrue(TEXT("Every member of the contacted aggregate is scheduled"),
		bEveryAggregateMemberScheduled);
	TestTrue(TEXT("Contact materialization remains bounded below world size"),
		WorldActor->GetGeneratedFragmentSourceCount()
			< Layout.FragmentSources.Num());
	FMatterFluxWorldSaveState LocalState;
	FString CaptureError;
	if (!TestTrue(TEXT("The contacted aggregate Volume state captures"),
		WorldActor->CaptureSaveState(LocalState, CaptureError)))
	{
		AddError(CaptureError);
		return false;
	}
	TestFalse(TEXT("No aggregate member stores object-level ReactionState"),
		LocalState.FragmentSources.ContainsByPredicate(
			[&AggregateMemberIds](const FMatterFluxSavedFragmentSourceState& State)
			{
				return State.bHasReactionState
					&& AggregateMemberIds.Contains(State.SourceId);
			}));
	TestTrue(TEXT("The root contact produces a sparse Volume material change"),
		LocalState.FragmentSources.ContainsByPredicate(
			[ContactedRootId](const FMatterFluxSavedFragmentSourceState& State)
			{
				return State.SourceId == ContactedRootId
					&& !State.VolumeCellStates.IsEmpty();
			}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterializedSourceReactionHandoffTest,
	"MatterFlux.Reaction.HotMaterializedVolumeRemainsScheduledUntilCool",
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
	if (!TestTrue(TEXT("A local fire element is deposited on the first tree"),
		WorldActor->ApplyMaterialStimulusToFirstGeneratedTree(731)))
	{
		return false;
	}
	WorldActor->Tick(0.06f);

	AFragment2DSourceActor* WoodSource = nullptr;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		if (!It->IsActorBeingDestroyed()
			&& It->SourceMaterialId == TEXT("wood")
			&& It->bAggregateRoot
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
	FFragment2DSourceStreamingState HotState;
	FString CaptureError;
	if (!TestTrue(TEXT("The hot trunk Volume state captures"),
		WoodSource->CaptureStreamingState(HotState, CaptureError)))
	{
		AddError(CaptureError);
		return false;
	}
	TestTrue(TEXT("Ignition creates a sparse non-environment energy field"),
		HotState.VolumeCellStates.ContainsByPredicate(
			[&HotState](const FFragment2DMaterialVolumeCellState& Cell)
			{
				return Cell.Energy != HotState.VolumeEnvironmentEnergy;
			}));
	TestFalse(TEXT("Streaming cannot deschedule a hot Volume"),
		WorldActor->DematerializeFragmentSource(SourceId));

	for (int32 Step = 0; Step < 30; ++Step)
	{
		WorldActor->Tick(0.1f);
	}

	AFragment2DSourceActor* ScheduledActor = nullptr;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		if (!It->IsActorBeingDestroyed() && It->SourceId == SourceId)
		{
			ScheduledActor = *It;
			break;
		}
	}
	TestNotNull(TEXT("The hot Volume remains in the local scheduler"),
		ScheduledActor);
	FMatterFluxWorldSaveState LocalState;
	if (!TestTrue(TEXT("The advanced local Volume state captures"),
		WorldActor->CaptureSaveState(LocalState, CaptureError)))
	{
		AddError(CaptureError);
		return false;
	}
	const FMatterFluxSavedFragmentSourceState* SavedSource =
		LocalState.FragmentSources.FindByPredicate(
			[SourceId](const FMatterFluxSavedFragmentSourceState& State)
			{
				return State.SourceId == SourceId;
			});
	if (!TestNotNull(TEXT("The scheduled Volume has a saved material state"),
		SavedSource))
	{
		return false;
	}
	TestTrue(TEXT("The scheduled trunk contains configured charcoal"),
		SavedSource->VolumeCellStates.ContainsByPredicate(
			[](const FMatterFluxSavedVolumeCellState& Cell)
			{
				return Cell.MaterialId == TEXT("charcoal");
			}));
	TestFalse(TEXT("The saved Volume still has no object ReactionState"),
		SavedSource->bHasReactionState);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxActiveAggregateMemberHandoffTest,
	"MatterFlux.Reaction.HotTreeMemberPreservesVolumeStateInDynamicCarrier",
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
	TArray<AFragment2DSourceActor*> Materialized;
	WorldActor->GatherFragmentSourcesInBounds(
		BuildWorldBounds(*Trunk),
		Materialized);
	AFragment2DSourceActor* HotLeafActor = nullptr;
	if (AFragment2DSourceActor** Found = Materialized.FindByPredicate(
		[Leaf](const AFragment2DSourceActor* Source)
		{
			return Source && Source->SourceId == Leaf->SourceId;
		}))
	{
		HotLeafActor = *Found;
	}
	if (!TestNotNull(TEXT("The contacted leaf Volume is materialized"),
		HotLeafActor))
	{
		return false;
	}
	FLocalMaterialReactionProgram Program;
	FString ProgramError;
	if (!TestTrue(TEXT("The local material program compiles for handoff"),
		Program.Compile(*Registry, ProgramError)))
	{
		AddError(ProgramError);
		return false;
	}
	FMaterialElementState LeafBefore;
	FMaterialElementState AshAfter;
	const FMatterFluxMaterialDefinition* LeafDefinition =
		Registry->Materials.Find(TEXT("leaf"));
	if (!TestNotNull(TEXT("The leaf thermal definition exists"), LeafDefinition)
		|| !TestTrue(TEXT("Leaf state resolves"),
			Program.MakeState(
				TEXT("leaf"), 255, TOptional<uint16>(), LeafBefore))
		|| !TestTrue(TEXT("Ash state resolves"),
			Program.MakeState(
				TEXT("ash"), 255,
				TOptional<uint16>(LeafDefinition
					? LeafDefinition->CombustionEnergy : 0),
				AshAfter)))
	{
		return false;
	}
	const int32 HotCellIndex = Leaf->Mask.SolidMask.IndexOfByKey(1);
	if (!TestTrue(TEXT("The leaf layer contains an occupied Volume cell"),
		HotCellIndex != INDEX_NONE))
	{
		return false;
	}
	const FIntVector HotCell(
		HotCellIndex % Leaf->Mask.Width,
		HotCellIndex / Leaf->Mask.Width,
		0);
	if (!TestTrue(TEXT("A high-energy ash cell commits atomically before felling"),
		HotLeafActor->CommitMaterialVolumeCellState(
			HotCell,
			LeafBefore.Energy,
			LeafBefore,
			AshAfter,
			ProgramError)))
	{
		AddError(ProgramError);
		return false;
	}
	FFragment2DSourceStreamingState HotLeafState;
	if (!TestTrue(TEXT("The hot leaf Volume state captures before felling"),
		HotLeafActor->CaptureStreamingState(HotLeafState, ProgramError)))
	{
		AddError(ProgramError);
		return false;
	}
	const FGuid HotLeafSourceId = HotLeafActor->SourceId;
	if (!TestTrue(TEXT("The hot leaf has sparse material or energy facts"),
		!HotLeafState.VolumeCellStates.IsEmpty()))
	{
		return false;
	}

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
		if (It->ContainsAggregateSource(HotLeafSourceId))
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
			&& It->SourceId == HotLeafSourceId;
	}
	TestFalse(
		TEXT("Active leaf does not fall back to an attached Source Actor"),
		bLeafActorRemains);
	if (!Carrier)
	{
		return false;
	}
	FFragmentAggregateSourceState CarriedLeafState;
	if (!TestTrue(TEXT("The carrier exposes the stable leaf SourceId"),
		Carrier->GetAggregateSourceState(HotLeafSourceId, CarriedLeafState)))
	{
		return false;
	}
	TestEqual(TEXT("Carrier preserves the Volume topology revision"),
		CarriedLeafState.VolumeTopologyRevision,
		HotLeafState.VolumeTopologyRevision);
	TestEqual(TEXT("Carrier preserves the Volume field revision"),
		CarriedLeafState.VolumeFieldRevision,
		HotLeafState.VolumeFieldRevision);
	TestEqual(TEXT("Carrier preserves every sparse Volume cell"),
		CarriedLeafState.VolumeCellStates.Num(),
		HotLeafState.VolumeCellStates.Num());
	for (int32 Index = 0; Index < HotLeafState.VolumeCellStates.Num(); ++Index)
	{
		const FFragment2DMaterialVolumeCellState& Before =
			HotLeafState.VolumeCellStates[Index];
		const FFragmentCarrierVolumeCellState& After =
			CarriedLeafState.VolumeCellStates[Index];
		TestEqual(TEXT("Carrier preserves the stable Volume address"),
			After.Cell, Before.Cell);
		TestEqual(TEXT("Carrier preserves the material identity"),
			After.MaterialId, Before.MaterialId);
		TestEqual(TEXT("Carrier preserves the specific energy"),
			After.Energy, Before.Energy);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxTerrainElementLocalReactionTest,
	"MatterFlux.Reaction.TerrainSpanElementsUseLocalKernelAndUnifiedState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxTerrainElementLocalReactionTest::RunTest(
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
	if (!TestTrue(TEXT("Reference terrain builds"),
		Registry.IsValid()
			&& MatterFlux::PlayableLevel::BuildLevelLayout(
				1337, Layout, Registry.Get())))
	{
		return false;
	}
	FIntPoint BareCell(INDEX_NONE, INDEX_NONE);
	for (int32 CandidateY = Layout.Terrain.Height / 4;
		CandidateY < Layout.Terrain.Height * 3 / 4
			&& BareCell.X == INDEX_NONE;
		CandidateY += 4)
	{
		for (int32 CandidateX = Layout.Terrain.Width / 4;
			CandidateX < Layout.Terrain.Width * 3 / 4;
			CandidateX += 4)
		{
			const FVector2D Candidate(
				Layout.Terrain.FirstCellCenter.X
					+ CandidateX * Layout.Terrain.CellSize,
				Layout.Terrain.FirstCellCenter.Y
					+ CandidateY * Layout.Terrain.CellSize);
			const bool bNearSource = Layout.FragmentSources.ContainsByPredicate(
				[Candidate](const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
				{
					return FVector2D::DistSquared(
						Candidate,
						FVector2D(Source.Transform.GetLocation()))
						< FMath::Square(500.0);
				});
			if (!bNearSource)
			{
				BareCell = FIntPoint(CandidateX, CandidateY);
				break;
			}
		}
	}
	if (!TestTrue(TEXT("Reference terrain contains a bare material cell"),
		BareCell.X != INDEX_NONE))
	{
		return false;
	}
	const int32 X = BareCell.X;
	const int32 Y = BareCell.Y;
	const FVector GroundWorld(
		Layout.Terrain.FirstCellCenter.X + X * Layout.Terrain.CellSize,
		Layout.Terrain.FirstCellCenter.Y + Y * Layout.Terrain.CellSize,
		Layout.Terrain.HeightAt(X, Y));
	TestEqual(TEXT("An ordinary fire element is deposited on bare terrain"),
		WorldActor->ApplyMaterialStimulusAtWorldLocation(
			GroundWorld, TEXT("fire"), 81021, 8.0f), 1);
	TestEqual(TEXT("Terrain does not mutate synchronously"),
		WorldActor->GetTerrainMaterialOverrideCellCount(), 0);
	for (int32 Step = 0; Step < 3; ++Step)
	{
		WorldActor->Tick(0.06f);
	}
	TestTrue(TEXT("The terrain cell reacts through the local kernel"),
		WorldActor->GetTerrainMaterialOverrideCellCount() > 0
			|| WorldActor->GetHotTerrainMaterialCellCount() > 0);
	TestTrue(TEXT("Terrain replication uses the unified span/field payload"),
		WorldActor->GetReplicatedTerrainMaterialByteCount() > 0);
	FMatterFluxWorldSaveState Saved;
	FString Error;
	if (!TestTrue(TEXT("World state captures after terrain reaction"),
		WorldActor->CaptureSaveState(Saved, Error)))
	{
		AddError(Error);
		return false;
	}
	TestFalse(TEXT("V6 never writes object-level Ground ReactionState"),
		Saved.bHasGroundReactionState);
	TestTrue(TEXT("V6 carries terrain material or energy facts by column"),
		Saved.TerrainSpanOverrides.ContainsByPredicate(
			[](const FMatterFluxTerrainSpanOverride& Column)
			{
				return Column.bHasTopologyOverride
					|| !Column.EnergyOverrides.IsEmpty();
			}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxDetachedWoodGroundIgnitionTest,
	"MatterFlux.Reaction.GroundParticleContactsDetachedVolumeWithoutReactionState",
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
	FLocalMaterialReactionProgram LocalProgram;
	FString ProgramError;
	if (!TestTrue(TEXT("Local material program compiles"),
		Registry.IsValid()
			&& LocalProgram.Compile(*Registry, ProgramError)))
	{
		AddError(ProgramError);
		return false;
	}
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
		WorldActor->GetTerrainMaterialOverrideCellCount();
	TestTrue(
		TEXT("A material particle is deposited beside detached wood"),
		WorldActor->SetSimulatedMaterialAtWorldLocation(
			GroundCellWorld,
			TEXT("fire")));
	TestFalse(
		TEXT("Depositing a particle does not synchronously mutate the carrier"),
		Carrier->IsRootMaterialHot());
	TestEqual(
		TEXT("Depositing a particle does not synchronously mutate ground"),
		WorldActor->GetTerrainMaterialOverrideCellCount(),
		GroundOutputBefore);
	WorldActor->Tick(0.06f);
	FMaterialElementAddress CarrierAddress;
	FMaterialElementState CarrierState;
	uint16 CarrierDefaultEnergy = 0;
	FVector CarrierCellCenter = FVector::ZeroVector;
	if (!TestTrue(TEXT("Detached wood remains a readable Volume element"),
		Carrier->TryGetMaterialVolumeElementAtWorldLocation(
			SourceTransform.GetLocation(),
			LocalProgram,
			CarrierAddress,
			CarrierState,
			CarrierDefaultEnergy,
			CarrierCellCenter)))
	{
		return false;
	}
	FName CarrierMaterial = NAME_None;
	LocalProgram.TryGetMaterialId(CarrierState.MaterialIndex, CarrierMaterial);
	TestTrue(TEXT("The ordinary fire element transforms detached wood locally"),
		CarrierMaterial == TEXT("charcoal")
			|| CarrierState.Energy != CarrierDefaultEnergy);
	TestEqual(TEXT("The local contact does not write legacy ground output"),
		WorldActor->GetTerrainMaterialOverrideCellCount(), GroundOutputBefore);
	TestEqual(TEXT("The local contact does not write legacy ground activity"),
		WorldActor->GetHotTerrainMaterialCellCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxDetachedCarrierInternalReactionBatchTest,
	"MatterFlux.Reaction.DetachedCarrierPropagatesLocalVolumeBatchAtomically",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxDetachedCarrierInternalReactionBatchTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	FLocalMaterialReactionProgram Program;
	FString Error;
	if (!TestTrue(TEXT("Local material program compiles"),
		Registry.IsValid() && Program.Compile(*Registry, Error)))
	{
		AddError(Error);
		return false;
	}

	constexpr float CellSize = 20.0f;
	MatterFlux::FragmentGeometry::FFragmentComponent Component;
	Component.Min = FIntPoint(0, 0);
	Component.Max = FIntPoint(1, 0);
	Component.Cells = {FIntPoint(0, 0), FIntPoint(1, 0)};
	TArray<FFragmentSpawnPayload> Payloads;
	const FTransform CarrierTransform(FVector(200.0f, 300.0f, 400.0f));
	if (!TestTrue(TEXT("Two-cell detached payload builds"),
		MatterFlux::FragmentGeometry::BuildSpawnPayloadsFromComponents(
			{Component},
			FGuid::NewDeterministicGuid(TEXT("CarrierInternalReaction"), 1),
			CarrierTransform,
			2,
			1,
			2,
			CellSize,
			1,
			1,
			CarrierTransform.GetLocation(),
			0.0f,
			8128,
			Payloads,
			EFragmentSourceGeometryStyle::VoxelBlocks))
		|| !TestEqual(TEXT("One connected payload exists"), Payloads.Num(), 1))
	{
		return false;
	}
	Payloads[0].MaterialId = TEXT("wood");
	Payloads[0].bEnableCollision = false;
	AFragment2DActor* Carrier = World->SpawnActor<AFragment2DActor>();
	if (!TestNotNull(TEXT("Detached carrier spawns"), Carrier)
		|| !TestTrue(TEXT("Detached carrier initializes"),
			Carrier->InitializeFromPayload(Payloads[0])))
	{
		return false;
	}

	const FVector LeftWorld = CarrierTransform.TransformPosition(
		FVector(-CellSize * 0.5f, 0.0f, 0.0f));
	const FVector RightWorld = CarrierTransform.TransformPosition(
		FVector(CellSize * 0.5f, 0.0f, 0.0f));
	FMaterialElementAddress LeftAddress;
	FMaterialElementAddress RightAddress;
	FMaterialElementState LeftBefore;
	FMaterialElementState RightBefore;
	uint16 LeftDefault = 0;
	uint16 RightDefault = 0;
	FVector CellCenter = FVector::ZeroVector;
	if (!TestTrue(TEXT("Left carrier cell resolves"),
		Carrier->TryGetMaterialVolumeElementAtWorldLocation(
			LeftWorld, Program, LeftAddress, LeftBefore, LeftDefault, CellCenter))
		|| !TestTrue(TEXT("Right carrier cell resolves"),
			Carrier->TryGetMaterialVolumeElementAtWorldLocation(
				RightWorld, Program, RightAddress, RightBefore, RightDefault, CellCenter)))
	{
		return false;
	}

	uint16 CharcoalIndex = 0;
	if (!TestTrue(TEXT("Charcoal has a compiled material index"),
		Program.TryGetMaterialIndex(TEXT("charcoal"), CharcoalIndex)))
	{
		return false;
	}
	FMaterialElementState HotLeft = LeftBefore;
	HotLeft.MaterialIndex = CharcoalIndex;
	HotLeft.Energy = 52000;

	// The second expected state is deliberately stale. The carrier must reject
	// the whole batch after validating the first delta without publishing it.
	FMaterialElementState StaleRight = RightBefore;
	++StaleRight.Energy;
	FMaterialDeltaBatch StaleBatch;
	StaleBatch.BaseStoreRevision = 0;
	StaleBatch.TargetStoreRevision = 1;
	StaleBatch.ExplicitEnergyDelta =
		HotLeft.GetTotalEnergy() - LeftBefore.GetTotalEnergy();
	StaleBatch.ElementDeltas = {
		{LeftAddress, LeftBefore, HotLeft},
		{RightAddress, StaleRight, StaleRight} };
	StaleBatch.ElementDeltas.Sort([](
		const FMaterialElementDelta& Left,
		const FMaterialElementDelta& Right)
	{
		return MaterialElementAddressLess(Left.Address, Right.Address);
	});
	const int32 TopologyBefore = Carrier->RootMaterialState.VolumeTopologyRevision;
	const int32 FieldsBefore = Carrier->RootMaterialState.VolumeFieldRevision;
	TestFalse(TEXT("A stale carrier batch is rejected atomically"),
		Carrier->CommitMaterialVolumeElementBatch(StaleBatch, Program, Error));
	FMaterialElementState LeftAfterRejected;
	TestTrue(TEXT("Rejected batch leaves the first cell readable"),
		Carrier->TryGetMaterialVolumeElementAtWorldLocation(
			LeftWorld, Program, LeftAddress, LeftAfterRejected, LeftDefault, CellCenter));
	TestTrue(TEXT("Rejected batch has zero cell side effects"),
		LeftAfterRejected == LeftBefore);
	TestEqual(TEXT("Rejected batch preserves topology revision"),
		Carrier->RootMaterialState.VolumeTopologyRevision, TopologyBefore);
	TestEqual(TEXT("Rejected batch preserves field revision"),
		Carrier->RootMaterialState.VolumeFieldRevision, FieldsBefore);

	FMaterialDeltaBatch HotBatch;
	HotBatch.BaseStoreRevision = 0;
	HotBatch.TargetStoreRevision = 1;
	HotBatch.ExplicitEnergyDelta =
		HotLeft.GetTotalEnergy() - LeftBefore.GetTotalEnergy();
	HotBatch.ElementDeltas.Add({LeftAddress, LeftBefore, HotLeft});
	const int32 MeshSectionsBeforeMaterialChange =
		Carrier->MeshComponent->GetNumSections();
	if (!TestTrue(TEXT("One hot material element commits"),
		Carrier->CommitMaterialVolumeElementBatch(HotBatch, Program, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("Sparse Volume material is projected into carrier mesh sections"),
		Carrier->MeshComponent->GetNumSections()
			> MeshSectionsBeforeMaterialChange);
	TArray<FTransform> DerivedFlames;
	TArray<MatterFlux::Rendering::FMaterialEmissionAnchor> DerivedSmoke;
	Carrier->GatherRootMaterialVisualTransforms(
		DerivedFlames, DerivedSmoke, 16);
	TestTrue(TEXT("Carrier flame presentation is derived from sparse energy"),
		!DerivedFlames.IsEmpty() && !DerivedSmoke.IsEmpty());
	const int32 TopologyAfterContact =
		Carrier->RootMaterialState.VolumeTopologyRevision;
	const int32 FieldsAfterContact =
		Carrier->RootMaterialState.VolumeFieldRevision;
	TArray<FMaterialParticleEmission> Emissions;
	int32 ProcessedContacts = 0;
	if (!TestTrue(TEXT("Carrier evaluates its internal contact through the kernel"),
		Carrier->AdvanceLocalMaterialVolumeReactions(
			Program, 1337, 1, 8, Emissions, ProcessedContacts, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Exactly one internal face contact is processed"),
		ProcessedContacts, 1);
	FMaterialElementState RightAfter;
	TestTrue(TEXT("Neighbour remains a stable readable element"),
		Carrier->TryGetMaterialVolumeElementAtWorldLocation(
			RightWorld, Program, RightAddress, RightAfter, RightDefault, CellCenter));
	FName RightMaterial = NAME_None;
	Program.TryGetMaterialId(RightAfter.MaterialIndex, RightMaterial);
	TestEqual(TEXT("Heat transfer ignites the neighbouring wood locally"),
		RightMaterial, FName(TEXT("charcoal")));
	TestEqual(TEXT("A multi-cell batch increments topology once"),
		Carrier->RootMaterialState.VolumeTopologyRevision,
		TopologyAfterContact + 1);
	TestEqual(TEXT("A multi-cell batch increments fields once"),
		Carrier->RootMaterialState.VolumeFieldRevision,
		FieldsAfterContact + 1);
	TestTrue(TEXT("Ignition emits an ordinary material particle"),
		!Emissions.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxCrossCarrierLocalReactionTest,
	"MatterFlux.Reaction.CrossCarrierContactCommitsAtomically",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxCrossCarrierLocalReactionTest::RunTest(
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
	FLocalMaterialReactionProgram Program;
	FString Error;
	if (!TestTrue(TEXT("Local material program compiles"),
		Registry.IsValid() && Program.Compile(*Registry, Error)))
	{
		AddError(Error);
		return false;
	}

	constexpr float CellSize = 20.0f;
	const auto SpawnCarrier = [World, CellSize](
		const TCHAR* StableName,
		const FVector& Location) -> AFragment2DActor*
	{
		MatterFlux::FragmentGeometry::FFragmentComponent Component;
		Component.Min = Component.Max = FIntPoint::ZeroValue;
		Component.Cells = {FIntPoint::ZeroValue};
		TArray<FFragmentSpawnPayload> Payloads;
		const FTransform Transform(Location);
		if (!MatterFlux::FragmentGeometry::BuildSpawnPayloadsFromComponents(
				{Component},
				FGuid::NewDeterministicGuid(StableName, 1),
				Transform,
				1,
				1,
				1,
				CellSize,
				1,
				1,
				Location,
				0.0f,
				9981,
				Payloads,
				EFragmentSourceGeometryStyle::VoxelBlocks)
			|| Payloads.Num() != 1)
		{
			return nullptr;
		}
		Payloads[0].MaterialId = TEXT("wood");
		Payloads[0].bEnableCollision = false;
		AFragment2DActor* Carrier = World->SpawnActor<AFragment2DActor>();
		return Carrier && Carrier->InitializeFromPayload(Payloads[0])
			? Carrier : nullptr;
	};
	const FVector LeftLocation(500.0f, 600.0f, 700.0f);
	const FVector RightLocation = LeftLocation + FVector(CellSize, 0.0f, 0.0f);
	AFragment2DActor* LeftCarrier = SpawnCarrier(
		TEXT("CrossCarrierLeft"), LeftLocation);
	AFragment2DActor* RightCarrier = SpawnCarrier(
		TEXT("CrossCarrierRight"), RightLocation);
	if (!TestNotNull(TEXT("Left carrier spawns"), LeftCarrier)
		|| !TestNotNull(TEXT("Right carrier spawns"), RightCarrier))
	{
		return false;
	}

	FMaterialElementAddress LeftAddress;
	FMaterialElementAddress RightAddress;
	FMaterialElementState LeftWood;
	FMaterialElementState RightWood;
	uint16 LeftDefault = 0;
	uint16 RightDefault = 0;
	FVector Center = FVector::ZeroVector;
	if (!LeftCarrier->TryGetMaterialVolumeElementAtWorldLocation(
			LeftLocation,
			Program,
			LeftAddress,
			LeftWood,
			LeftDefault,
			Center)
		|| !RightCarrier->TryGetMaterialVolumeElementAtWorldLocation(
			RightLocation,
			Program,
			RightAddress,
			RightWood,
			RightDefault,
			Center))
	{
		AddError(TEXT("Cross-carrier Volume cells did not resolve"));
		return false;
	}
	uint16 CharcoalIndex = 0;
	Program.TryGetMaterialIndex(TEXT("charcoal"), CharcoalIndex);
	FMaterialElementState HotLeft = LeftWood;
	HotLeft.MaterialIndex = CharcoalIndex;
	HotLeft.Energy = 52000;
	FMaterialDeltaBatch HotBatch;
	HotBatch.BaseStoreRevision = 0;
	HotBatch.TargetStoreRevision = 1;
	HotBatch.ExplicitEnergyDelta =
		HotLeft.GetTotalEnergy() - LeftWood.GetTotalEnergy();
	HotBatch.ElementDeltas.Add({LeftAddress, LeftWood, HotLeft});
	if (!TestTrue(TEXT("Left carrier accepts one hot element"),
		LeftCarrier->CommitMaterialVolumeElementBatch(
			HotBatch, Program, Error)))
	{
		AddError(Error);
		return false;
	}

	TArray<FFragmentCarrierMaterialElement> LeftElements;
	TArray<FFragmentCarrierMaterialElement> RightElements;
	if (!LeftCarrier->GatherMaterialVolumeElements(
			Program, LeftElements, Error)
		|| !RightCarrier->GatherMaterialVolumeElements(
			Program, RightElements, Error)
		|| LeftElements.Num() != 1 || RightElements.Num() != 1)
	{
		AddError(Error.IsEmpty()
			? TEXT("Cross-carrier immutable projections are incomplete")
			: Error);
		return false;
	}
	FLocalMaterialReactionContext Context;
	Context.Seed = 1337;
	Context.LogicalStep = 1;
	Context.MaxContacts = 1;
	Context.MaxElementDeltas = 2;
	Context.MaxEmissions = 2;
	FMaterialDeltaBatch PairBatch;
	if (!TestTrue(TEXT("Cross-carrier pair evaluates through the kernel"),
		Program.EvaluatePair(
			LeftElements[0].Address,
			LeftElements[0].State,
			RightElements[0].Address,
			RightElements[0].State,
			0,
			Context,
			PairBatch,
			Error)))
	{
		AddError(Error);
		return false;
	}

	FMaterialElementState ChangedRight = RightElements[0].State;
	++ChangedRight.Energy;
	if (!TestTrue(TEXT("Right carrier changes after immutable evaluation"),
		RightCarrier->CommitMaterialVolumeCellState(
			RightAddress,
			RightDefault,
			RightElements[0].State,
			ChangedRight,
			Program,
			Error)))
	{
		AddError(Error);
		return false;
	}
	TestFalse(TEXT("Stale cross-carrier pair is rejected"),
		AFragment2DActor::CommitMaterialVolumePairBatch(
			*LeftCarrier, *RightCarrier, PairBatch, Program, Error));
	FMaterialElementState LeftAfterRollback;
	TestTrue(TEXT("Left element remains readable after pair rollback"),
		LeftCarrier->TryGetMaterialVolumeElementAtWorldLocation(
			LeftLocation,
			Program,
			LeftAddress,
			LeftAfterRollback,
			LeftDefault,
			Center));
	TestTrue(TEXT("Stale second carrier rolls the first carrier back"),
		LeftAfterRollback == LeftElements[0].State);
	TestFalse(TEXT("A single-cell hot carrier has no internal contact work"),
		LeftCarrier->HasLocalMaterialVolumeReactionWork());
	TestTrue(TEXT("Its non-environment energy still schedules cross contact"),
		LeftCarrier->HasNonEnvironmentMaterialVolumeEnergy());

	WorldActor->Tick(0.06f);
	FMaterialElementState RightAfterWorldStep;
	TestTrue(TEXT("Right element remains readable after world fixed step"),
		RightCarrier->TryGetMaterialVolumeElementAtWorldLocation(
			RightLocation,
			Program,
			RightAddress,
			RightAfterWorldStep,
			RightDefault,
			Center));
	FName RightMaterial = NAME_None;
	Program.TryGetMaterialId(
		RightAfterWorldStep.MaterialIndex, RightMaterial);
	TestEqual(TEXT("Adjacent rigid carrier is ignited by local contact"),
		RightMaterial, FName(TEXT("charcoal")));
	TestTrue(TEXT("Right carrier stores the local result as sparse Volume facts"),
		!RightCarrier->RootMaterialState.VolumeCellStates.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxDetachedTreeIgnitionTest,
	"MatterFlux.Reaction.DetachedTreeVolumesReactLocallyAfterFelling",
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
	FLocalMaterialReactionProgram LocalProgram;
	FString ProgramError;
	if (!TestTrue(TEXT("Local material program compiles"),
		Registry.IsValid()
			&& LocalProgram.Compile(*Registry, ProgramError)))
	{
		AddError(ProgramError);
		return false;
	}
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
		Carrier->RootMaterialState.MaterialId,
		FName(TEXT("wood")));
	TestTrue(
		TEXT("Detached root exposes a valid reactive voxel mask"),
		Carrier->RootMaterialState.IsValid());
	const FBox CarrierBounds = Carrier->GetReactiveWorldBounds();
	TestTrue(
		TEXT("Detached carrier exposes deterministic reactive bounds"),
		CarrierBounds.IsValid != 0);
	const FFragmentSourceMask& RootMask =
		Carrier->RootMaterialState.SourceMask;
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
	TestTrue(TEXT("A stable fire element is authored at the trunk contact"),
		WorldActor->SetSimulatedMaterialAtWorldLocation(
			RootHit.ImpactPoint, TEXT("fire")));
	TestFalse(
		TEXT("Particle impact does not synchronously activate the trunk"),
		Carrier->IsRootMaterialHot());
	WorldActor->Tick(0.06f);
	const auto ReadCarrierElement = [Carrier, &LocalProgram](
		const FVector& WorldLocation,
		FMaterialElementAddress& OutAddress,
		FMaterialElementState& OutState,
		uint16& OutDefaultEnergy,
		FName& OutMaterial)
	{
		FVector CellCenter = FVector::ZeroVector;
		if (!Carrier->TryGetMaterialVolumeElementAtWorldLocation(
				WorldLocation,
				LocalProgram,
				OutAddress,
				OutState,
				OutDefaultEnergy,
				CellCenter))
		{
			return false;
		}
		return LocalProgram.TryGetMaterialId(
			OutState.MaterialIndex, OutMaterial);
	};
	FMaterialElementAddress RootAddress;
	FMaterialElementState RootState;
	uint16 RootDefaultEnergy = 0;
	FName RootMaterial = NAME_None;
	TestTrue(TEXT("Normal wand contact leaves a readable trunk Volume cell"),
		ReadCarrierElement(
			RootHit.ImpactPoint,
			RootAddress,
			RootState,
			RootDefaultEnergy,
			RootMaterial));
	const bool bAnyDetachedWoodChanged =
		RootMaterial == TEXT("charcoal")
		|| RootState.Energy != RootDefaultEnergy
		|| Carrier->RootMaterialState.VolumeCellStates.ContainsByPredicate(
			[](const FFragmentCarrierVolumeCellState& Cell)
			{
				return Cell.MaterialId == TEXT("charcoal");
			})
		|| Carrier->AggregateSources.ContainsByPredicate(
			[](const FFragmentAggregateSourceState& Source)
			{
				return Source.MaterialId == TEXT("wood")
					&& Source.VolumeCellStates.ContainsByPredicate(
						[](const FFragmentCarrierVolumeCellState& Cell)
						{
							return Cell.MaterialId == TEXT("charcoal");
						});
			});
	TestTrue(TEXT("Normal wand material transforms one contacted trunk Volume"),
		bAnyDetachedWoodChanged);
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
	TestTrue(TEXT("A stable fire element is authored at the leaf contact"),
		WorldActor->SetSimulatedMaterialAtWorldLocation(
			LeafHit.ImpactPoint, TEXT("fire")));
	WorldActor->Tick(0.06f);
	FMaterialElementAddress LeafAddress;
	FMaterialElementState LeafState;
	uint16 LeafDefaultEnergy = 0;
	FName LeafMaterial = NAME_None;
	TestTrue(TEXT("Detached leaf remains a readable Volume element"),
		ReadCarrierElement(
			LeafHit.ImpactPoint,
			LeafAddress,
			LeafState,
			LeafDefaultEnergy,
			LeafMaterial));
	TestEqual(TEXT("The stable leaf address survives felling"),
		LeafAddress.OwnerId, Leaf->SourceId);
	TestTrue(TEXT("A detached leaf transforms through the local kernel"),
		LeafMaterial == TEXT("ash")
			|| LeafState.Energy != LeafDefaultEnergy);
	TestTrue(
		TEXT("A material particle is deposited on the cut wood layer"),
		WorldActor->SetSimulatedMaterialAtWorldLocation(
			CutWoodWorldLocation,
			TEXT("fire")));
	WorldActor->Tick(0.06f);
	FMaterialElementAddress CutWoodAddress;
	FMaterialElementState CutWoodState;
	uint16 CutWoodDefaultEnergy = 0;
	FName CutWoodMaterial = NAME_None;
	TestTrue(TEXT("Cut wood remains a readable Volume element"),
		ReadCarrierElement(
			CutWoodWorldLocation,
			CutWoodAddress,
			CutWoodState,
			CutWoodDefaultEnergy,
			CutWoodMaterial));
	TestEqual(TEXT("Cut wood keeps its stable independent Volume owner"),
		CutWoodAddress.OwnerId, CutWoodLayerId);
	TestTrue(TEXT("Cut wood transforms independently through the local kernel"),
		CutWoodMaterial == TEXT("charcoal")
			|| CutWoodState.Energy != CutWoodDefaultEnergy);

	for (int32 Step = 0; Step < 60; ++Step)
	{
		Carrier->Tick(0.1f);
		WorldActor->Tick(0.1f);
	}
	FFragmentAggregateSourceState BurnedCutWoodLayer;
	TestTrue(
		TEXT("The cut wood layer remains addressable after active"),
		Carrier->GetAggregateSourceState(
			CutWoodLayerId,
			BurnedCutWoodLayer));
	TestTrue(TEXT("Cut wood persists its sparse local material fact"),
		BurnedCutWoodLayer.VolumeCellStates.ContainsByPredicate(
			[](const FFragmentCarrierVolumeCellState& Cell)
			{
				return Cell.MaterialId == TEXT("charcoal");
			}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxDecorationReactionIntegrationTest,
	"MatterFlux.Reaction.DecorationMaskBurnsThroughLuaRule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxDecorationReactionIntegrationTest::RunTest(
	const FString& Parameters)
{
	FAutomationEditorCommonUtils::CreateNewMap();
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!TestTrue(TEXT("Active Lua material registry exists"), Registry.IsValid()))
	{
		return false;
	}
	FLocalMaterialReactionProgram Program;
	FString Error;
	if (!TestTrue(TEXT("Lua materials compile into the local reaction program"),
		Program.Compile(*Registry, Error)))
	{
		AddError(Error);
		return false;
	}

	FMaterialElementState WoodState;
	FMaterialElementState FireState;
	TestTrue(TEXT("Wood resolves to one material element"),
		Program.MakeState(TEXT("wood"), 255, TOptional<uint16>(), WoodState));
	TestTrue(TEXT("Fire resolves to one airborne material element"),
		Program.MakeState(TEXT("fire"), 255, TOptional<uint16>(), FireState));
	const FMaterialElementAddress WoodAddress =
		FMaterialElementAddress::MakeVolumeCell(
			FGuid::NewDeterministicGuid(TEXT("DecorationLocalReaction"), 1),
			FIntVector::ZeroValue);
	const FMaterialElementAddress FireAddress =
		FMaterialElementAddress::MakeAirborneParticle(
			FGuid::NewDeterministicGuid(TEXT("DecorationLocalReaction"), 2));
	FLocalMaterialReactionContext Context;
	Context.Seed = 404;
	Context.LogicalStep = 1;
	Context.MaxContacts = 1;
	Context.MaxElementDeltas = 2;
	Context.MaxEmissions = 2;
	Context.bApplyCooling = false;
	FMaterialDeltaBatch Batch;
	if (!TestTrue(TEXT("Local fire contact evaluates atomically"),
		Program.EvaluatePair(
			WoodAddress, WoodState,
			FireAddress, FireState,
			0, Context, Batch, Error)))
	{
		AddError(Error);
		return false;
	}
	const FMaterialElementDelta* WoodDelta = Batch.ElementDeltas.FindByPredicate(
		[&WoodAddress](const FMaterialElementDelta& Delta)
		{
			return Delta.Address == WoodAddress;
		});
	if (!TestNotNull(TEXT("Burning produces a wood-cell delta"), WoodDelta))
	{
		return false;
	}
	FName ProductId;
	TestTrue(TEXT("Wood combustion product resolves"),
		Program.TryGetMaterialId(WoodDelta->After.MaterialIndex, ProductId));
	TestEqual(TEXT("Wood becomes configured charcoal"),
		ProductId, FName(TEXT("charcoal")));
	uint16 SmokeIndex = 0;
	TestTrue(TEXT("Smoke material resolves"),
		Program.TryGetMaterialIndex(TEXT("smoke"), SmokeIndex));
	TestTrue(TEXT("Wood combustion emits an ordinary smoke element"),
		Batch.ParticleEmissions.ContainsByPredicate(
			[SmokeIndex](const FMaterialParticleEmission& Emission)
			{
				return Emission.MaterialIndex == SmokeIndex;
			}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxForestPlantReactionTest,
	"MatterFlux.Reaction.LeavesGrassAndFlowersUseConfiguredReactions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxForestPlantReactionTest::RunTest(
	const FString& Parameters)
{
	FAutomationEditorCommonUtils::CreateNewMap();
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!TestTrue(TEXT("Active Lua material registry exists"), Registry.IsValid()))
	{
		return false;
	}
	FLocalMaterialReactionProgram Program;
	FString Error;
	if (!TestTrue(TEXT("Plant thermal definitions compile"),
		Program.Compile(*Registry, Error)))
	{
		AddError(Error);
		return false;
	}
	FMaterialElementState FireState;
	if (!TestTrue(TEXT("Fire material resolves"),
		Program.MakeState(TEXT("fire"), 255, TOptional<uint16>(), FireState)))
	{
		return false;
	}
	uint16 AshIndex = 0;
	uint16 SmokeIndex = 0;
	TestTrue(TEXT("Ash material resolves"),
		Program.TryGetMaterialIndex(TEXT("ash"), AshIndex));
	TestTrue(TEXT("Smoke material resolves"),
		Program.TryGetMaterialIndex(TEXT("smoke"), SmokeIndex));
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
		FMaterialElementState PlantState;
		if (!TestTrue(
			*FString::Printf(TEXT("%s material resolves"),
				*InputMaterials[InputIndex].ToString()),
			Program.MakeState(
				InputMaterials[InputIndex], 255,
				TOptional<uint16>(), PlantState)))
		{
			continue;
		}
		const FMaterialElementAddress PlantAddress =
			FMaterialElementAddress::MakeVolumeCell(
				FGuid::NewDeterministicGuid(
					TEXT("ForestPlantLocalReaction"), InputIndex + 1),
				FIntVector::ZeroValue);
		const FMaterialElementAddress FireAddress =
			FMaterialElementAddress::MakeAirborneParticle(
				FGuid::NewDeterministicGuid(
					TEXT("ForestPlantLocalReactionFire"), InputIndex + 1));
		FLocalMaterialReactionContext Context;
		Context.Seed = 500 + InputIndex;
		Context.LogicalStep = 1;
		Context.MaxContacts = 1;
		Context.MaxElementDeltas = 2;
		Context.MaxEmissions = 2;
		Context.bApplyCooling = false;
		FMaterialDeltaBatch Batch;
		if (!TestTrue(
			*FString::Printf(TEXT("%s reacts through local contact"),
				*InputMaterials[InputIndex].ToString()),
			Program.EvaluatePair(
				PlantAddress, PlantState,
				FireAddress, FireState,
				0, Context, Batch, Error)))
		{
			AddError(Error);
			continue;
		}
		const FMaterialElementDelta* PlantDelta =
			Batch.ElementDeltas.FindByPredicate(
				[&PlantAddress](const FMaterialElementDelta& Delta)
				{
					return Delta.Address == PlantAddress;
				});
		TestTrue(
			*FString::Printf(TEXT("%s produces a material delta"),
				*InputMaterials[InputIndex].ToString()),
			PlantDelta != nullptr);
		if (PlantDelta)
		{
			TestEqual(
				*FString::Printf(TEXT("%s becomes configured ash"),
					*InputMaterials[InputIndex].ToString()),
				PlantDelta->After.MaterialIndex, AshIndex);
		}
		TestTrue(
			*FString::Printf(TEXT("%s emits ordinary smoke"),
				*InputMaterials[InputIndex].ToString()),
			Batch.ParticleEmissions.ContainsByPredicate(
				[SmokeIndex](const FMaterialParticleEmission& Emission)
				{
					return Emission.MaterialIndex == SmokeIndex;
				}));
	}
	return true;
}
