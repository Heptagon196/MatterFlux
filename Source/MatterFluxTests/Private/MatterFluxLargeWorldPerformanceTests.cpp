#include "Fragment/Fragment2DSourceActor.h"
#include "Game/MatterFluxCharacter.h"
#include "Game/MatterFluxPlayableLevel.h"
#include "Game/MatterFluxPlayableWorldActor.h"

#include "Algo/Sort.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
	enum class EMatterFluxLongTraversalPhase : uint8
	{
		Warmup,
		Traverse,
		Drain
	};

	struct FMatterFluxLongTraversalState
	{
		FAutomationTestBase* Test = nullptr;
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<AMatterFluxPlayableWorldActor> WorldActor;
		TWeakObjectPtr<AMatterFluxCharacter> Character;
		EMatterFluxLongTraversalPhase Phase =
			EMatterFluxLongTraversalPhase::Warmup;
		FVector PathStart = FVector::ZeroVector;
		FVector PathEnd = FVector::ZeroVector;
		FIntPoint PreviousChunk = FIntPoint::ZeroValue;
		float ChunkWorldSize = 0.0f;
		int32 MovementFrames = 0;
		int32 WarmupFrames = 0;
		int32 MovementFrame = 0;
		int32 DrainFrames = 0;
		int32 BoundaryCrossingCount = 0;
		int32 SlowPlayableWorldTickCount = 0;
		int32 VisibleHitchFrameCount = 0;
		int32 SuspendedMetricSampleCount = 0;
		int32 MissingTerrainFrameCount = 0;
		int32 MissingPopulationFrameCount = 0;
		int32 MaximumPopulationBacklog = 0;
		int32 MaximumTerrainBacklog = 0;
		int32 MaximumStreamedHouseCount = 0;
		double PreviousFrameStartSeconds = 0.0;
		double MaximumPlayableWorldTickMilliseconds = 0.0;
		double MaximumFrameIntervalMilliseconds = 0.0;
		TArray<double> PlayableWorldTickSamples;
		TArray<double> FrameIntervalSamples;
		bool bTraceRegionOpen = false;
	};

	class FMatterFluxLongTraversalLatentCommand final
		: public IAutomationLatentCommand
	{
	public:
		explicit FMatterFluxLongTraversalLatentCommand(
			TSharedPtr<FMatterFluxLongTraversalState> InState)
			: State(MoveTemp(InState))
		{
		}

		virtual bool Update() override
		{
			if (!State.IsValid() || !State->Test)
			{
				return true;
			}
			UWorld* World = State->World.Get();
			AMatterFluxPlayableWorldActor* WorldActor =
				State->WorldActor.Get();
			AMatterFluxCharacter* Character = State->Character.Get();
			if (!World || !WorldActor || !Character)
			{
				CloseTraceRegion();
				State->Test->AddError(
					TEXT("Long traversal lost its world or player"));
				return true;
			}

			switch (State->Phase)
			{
			case EMatterFluxLongTraversalPhase::Warmup:
				WorldActor->Tick(FixedDeltaSeconds);
				++State->WarmupFrames;
				if (StreamingIsIdle(*WorldActor))
				{
					State->Phase = EMatterFluxLongTraversalPhase::Traverse;
					State->PreviousFrameStartSeconds =
						FPlatformTime::Seconds();
					TRACE_BEGIN_REGION(
						TEXT("MatterFluxLongContinuousExternalTraversal"));
					State->bTraceRegionOpen = true;
				}
				else if (State->WarmupFrames >= MaximumWarmupFrames)
				{
					State->Test->AddError(FString::Printf(
						TEXT("Long traversal warmup did not drain: entry_ready=%d population=%d terrain=%d fragment=%d"),
						WorldActor->IsInitialWorldEntryReady() ? 1 : 0,
						WorldActor->GetPendingProceduralPopulationUpdateCount(),
						WorldActor->GetPendingTerrainChunkPrefetchCount(),
						WorldActor->GetPendingFragmentSourceSpawnCount()));
					return true;
				}
				return false;

			case EMatterFluxLongTraversalPhase::Traverse:
				return UpdateTraversal(*WorldActor, *Character);

			case EMatterFluxLongTraversalPhase::Drain:
				WorldActor->Tick(FixedDeltaSeconds);
				++State->DrainFrames;
				if (StreamingIsIdle(*WorldActor)
					|| State->DrainFrames >= MaximumDrainFrames)
				{
					return Finish(*WorldActor);
				}
				return false;
			}
			return true;
		}

	private:
		static constexpr float FixedDeltaSeconds = 1.0f / 60.0f;
		static constexpr int32 MaximumWarmupFrames = 1200;
		static constexpr int32 MaximumDrainFrames = 1800;
		static constexpr double SixtyFpsBudgetMilliseconds = 16.67;
		static constexpr double TwoFrameBudgetMilliseconds = 33.34;
		static constexpr double VisibleHitchMilliseconds = 250.0;
		static constexpr double ProcessSuspensionMilliseconds = 30000.0;

		static bool StreamingIsIdle(
			const AMatterFluxPlayableWorldActor& WorldActor)
		{
			return WorldActor.IsInitialWorldEntryReady()
				&& WorldActor.GetPendingFragmentSourceSpawnCount() == 0
				&& WorldActor.GetPendingProceduralPopulationUpdateCount() == 0
				&& WorldActor.GetPendingTerrainChunkPrefetchCount() == 0;
		}

		FIntPoint ToChunk(const FVector& Location) const
		{
			return FIntPoint(
				FMath::FloorToInt(Location.X / State->ChunkWorldSize),
				FMath::FloorToInt(Location.Y / State->ChunkWorldSize));
		}

		bool UpdateTraversal(
			AMatterFluxPlayableWorldActor& WorldActor,
			AMatterFluxCharacter& Character)
		{
			const double FrameStartSeconds = FPlatformTime::Seconds();
			if (State->MovementFrame > 0)
			{
				const double FrameIntervalMilliseconds =
					(FrameStartSeconds - State->PreviousFrameStartSeconds)
						* 1000.0;
				if (FrameIntervalMilliseconds < ProcessSuspensionMilliseconds)
				{
					State->FrameIntervalSamples.Add(FrameIntervalMilliseconds);
					State->MaximumFrameIntervalMilliseconds = FMath::Max(
						State->MaximumFrameIntervalMilliseconds,
						FrameIntervalMilliseconds);
					State->VisibleHitchFrameCount +=
						FrameIntervalMilliseconds >= VisibleHitchMilliseconds ? 1 : 0;
				}
				else
				{
					++State->SuspendedMetricSampleCount;
				}
			}

			if (State->MovementFrame >= State->MovementFrames)
			{
				CloseTraceRegion();
				State->Phase = EMatterFluxLongTraversalPhase::Drain;
				return false;
			}

			State->PreviousFrameStartSeconds = FrameStartSeconds;
			++State->MovementFrame;
			const FVector Location = FMath::Lerp(
				State->PathStart,
				State->PathEnd,
				static_cast<float>(State->MovementFrame)
					/ static_cast<float>(State->MovementFrames));
			const FIntPoint CurrentChunk = ToChunk(Location);
			const bool bBoundaryFrame = CurrentChunk != State->PreviousChunk;
			Character.SetActorLocation(Location);
			const double PlayableWorldTickStartSeconds = FPlatformTime::Seconds();
			WorldActor.Tick(FixedDeltaSeconds);
			const double PlayableWorldTickMilliseconds =
				(FPlatformTime::Seconds() - PlayableWorldTickStartSeconds) * 1000.0;
			if (PlayableWorldTickMilliseconds < ProcessSuspensionMilliseconds)
			{
				State->PlayableWorldTickSamples.Add(PlayableWorldTickMilliseconds);
				State->MaximumPlayableWorldTickMilliseconds = FMath::Max(
					State->MaximumPlayableWorldTickMilliseconds,
					PlayableWorldTickMilliseconds);
				State->SlowPlayableWorldTickCount +=
					PlayableWorldTickMilliseconds >= SixtyFpsBudgetMilliseconds ? 1 : 0;
			}
			else
			{
				++State->SuspendedMetricSampleCount;
			}
			State->BoundaryCrossingCount += bBoundaryFrame ? 1 : 0;
			State->MissingTerrainFrameCount +=
				WorldActor.HasVisibleTerrainChunk(CurrentChunk) ? 0 : 1;
			State->MissingPopulationFrameCount +=
				WorldActor.HasProceduralPopulationChunk(CurrentChunk) ? 0 : 1;
			State->MaximumPopulationBacklog = FMath::Max(
				State->MaximumPopulationBacklog,
				WorldActor.GetPendingProceduralPopulationUpdateCount());
			State->MaximumTerrainBacklog = FMath::Max(
				State->MaximumTerrainBacklog,
				WorldActor.GetPendingTerrainChunkPrefetchCount());
			State->MaximumStreamedHouseCount = FMath::Max(
				State->MaximumStreamedHouseCount,
				WorldActor.GetGeneratedStreamedHouseCount());
			State->PreviousChunk = CurrentChunk;
			return false;
		}

		static double Percentile(
			TArray<double> Samples,
			const double Fraction)
		{
			if (Samples.IsEmpty())
			{
				return 0.0;
			}
			Algo::Sort(Samples);
			const int32 Index = FMath::Clamp(
				FMath::CeilToInt(Samples.Num() * Fraction) - 1,
				0,
				Samples.Num() - 1);
			return Samples[Index];
		}

		bool Finish(AMatterFluxPlayableWorldActor& WorldActor)
		{
			CloseTraceRegion();
			const int32 Radius = WorldActor.GetTerrainStreamingChunkRadius();
			const int32 Diameter = Radius * 2 + 1;
			const int32 CameraOverlapSide = FMath::Max(Diameter - 2, 0);
			const int32 ExpectedVisibleChunks =
				Diameter * Diameter * 2
					- CameraOverlapSide * CameraOverlapSide;
			const int32 PopulationRadius = Radius + 1;
			const int32 PopulationDiameter = PopulationRadius * 2 + 1;
			const int32 PopulationCameraOverlapSide =
				FMath::Max(PopulationDiameter - 2, 0);
			const int32 ExpectedPopulationChunks =
				PopulationDiameter * PopulationDiameter * 2
					- PopulationCameraOverlapSide
						* PopulationCameraOverlapSide;
			const int32 ProceduralChunkCount =
				WorldActor.GetProceduralPopulationChunkCount();
			const int32 TreeCount =
				WorldActor.GetProceduralTreeAggregateCount();
			const double TreesPerChunk = ProceduralChunkCount > 0
				? static_cast<double>(TreeCount) / ProceduralChunkCount
				: 0.0;

			TArray<AFragment2DSourceActor*> FinalSources;
			const FVector SearchExtent(
				Radius * State->ChunkWorldSize,
				Radius * State->ChunkWorldSize,
				2000.0f);
			WorldActor.GatherFragmentSourcesInBounds(
				FBox(State->PathEnd - SearchExtent, State->PathEnd + SearchExtent),
				FinalSources);
			TSet<FName> FinalMaterials;
			for (const AFragment2DSourceActor* Source : FinalSources)
			{
				if (Source
					&& Source->ActorHasTag(TEXT("MatterFluxGeneratedDecoration")))
				{
					FinalMaterials.Add(Source->SourceMaterialId);
				}
			}

			const double PlayableWorldTickP99 =
				Percentile(State->PlayableWorldTickSamples, 0.99);
			const double FrameIntervalP99 =
				Percentile(State->FrameIntervalSamples, 0.99);
			State->Test->AddInfo(FString::Printf(
				TEXT("Long real-frame external walk: %d frames, %d boundaries; playable-world tick p99 %.2f ms max %.2f ms slow=%d; frame interval p99 %.2f ms max %.2f ms visible hitches=%d; suspended samples discarded=%d; missing terrain=%d population=%d; max backlogs population=%d terrain=%d; resident=%d/%d trees=%d (%.3f/chunk), houses=%d, final materials=%d"),
				State->MovementFrames,
				State->BoundaryCrossingCount,
				PlayableWorldTickP99,
				State->MaximumPlayableWorldTickMilliseconds,
				State->SlowPlayableWorldTickCount,
				FrameIntervalP99,
				State->MaximumFrameIntervalMilliseconds,
				State->VisibleHitchFrameCount,
				State->SuspendedMetricSampleCount,
				State->MissingTerrainFrameCount,
				State->MissingPopulationFrameCount,
				State->MaximumPopulationBacklog,
				State->MaximumTerrainBacklog,
				ProceduralChunkCount,
				ExpectedPopulationChunks,
				TreeCount,
				TreesPerChunk,
				State->MaximumStreamedHouseCount,
				FinalMaterials.Num()));

			State->Test->TestEqual(
				TEXT("Long traversal crosses every requested chunk boundary"),
				State->BoundaryCrossingCount,
				64);
			State->Test->TestEqual(
				TEXT("Playable-world streaming ticks stay inside the 60 FPS budget"),
				State->SlowPlayableWorldTickCount,
				0);
			State->Test->TestTrue(
				TEXT("99 percent of rendered frame intervals stay inside two 60 FPS frames"),
				FrameIntervalP99 < TwoFrameBudgetMilliseconds);
			State->Test->TestEqual(
				TEXT("Long traversal has no visibly frozen frame"),
				State->VisibleHitchFrameCount,
				0);
			State->Test->TestEqual(
				TEXT("Terrain is resident before the player reaches each chunk"),
				State->MissingTerrainFrameCount,
				0);
			State->Test->TestEqual(
				TEXT("Decorations are resident before the player reaches each external chunk"),
				State->MissingPopulationFrameCount,
				0);
			State->Test->TestTrue(
				TEXT("The final terrain window is completely visible"),
				WorldActor.GetVisibleTerrainChunkCount()
					== ExpectedVisibleChunks);
			State->Test->TestEqual(
				TEXT("The final off-screen decoration prefetch ring is resident"),
				ProceduralChunkCount,
				ExpectedPopulationChunks);
			State->Test->TestTrue(
				TEXT("The final external forest keeps local authored density"),
				TreesPerChunk >= 1.5);
			State->Test->TestTrue(
				TEXT("Long traversal encounters streamed houses"),
				State->MaximumStreamedHouseCount > 0);
			const FName RequiredMaterials[] = {
				TEXT("wood"),
				TEXT("leaf"),
				TEXT("stone"),
				TEXT("grass"),
				TEXT("flower_pink"),
				TEXT("flower_gold"),
				TEXT("flower_blue")
			};
			for (const FName Material : RequiredMaterials)
			{
				State->Test->TestTrue(
					*FString::Printf(
						TEXT("The final streamed window visibly contains %s"),
						*Material.ToString()),
					FinalMaterials.Contains(Material));
			}
			State->Test->TestEqual(
				TEXT("Long traversal drains the decoration queue"),
				WorldActor.GetPendingProceduralPopulationUpdateCount(),
				0);
			State->Test->TestEqual(
				TEXT("Long traversal drains the terrain queue"),
				WorldActor.GetPendingTerrainChunkPrefetchCount(),
				0);
			return true;
		}

		void CloseTraceRegion()
		{
			if (State.IsValid() && State->bTraceRegionOpen)
			{
				TRACE_END_REGION(TEXT("MatterFluxLongContinuousExternalTraversal"));
				State->bTraceRegionOpen = false;
			}
		}

		TSharedPtr<FMatterFluxLongTraversalState> State;
	};
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
		TArray<uint8> RuntimeMask;
		RuntimeMask.SetNumUninitialized(256);
		for (int32 CellIndex = 0; CellIndex < 256; ++CellIndex)
		{
			RuntimeMask[CellIndex] =
				((CellIndex + Pattern) & 1) != 0 ? 1 : 0;
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
		SourceCount * 32);
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
			2),
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
			2),
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
			2),
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
			2),
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
		2);

	States.ResetAuthorityItems();
	TestTrue(TEXT("Reset removes all replicated Source states"), States.Items.IsEmpty());
	TestEqual(
		TEXT("Reset clears the cached byte count"),
		States.GetAuthorityPayloadByteCount(),
		0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxInfiniteBoundaryStreamingPerformanceTest,
	"MatterFlux.Performance.InfinitePopulationBoundaryFrame",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::PerfFilter)

bool FMatterFluxInfiniteBoundaryStreamingPerformanceTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor =
		World ? World->SpawnActor<AMatterFluxPlayableWorldActor>() : nullptr;
	if (!TestNotNull(TEXT("Playable world actor spawned"), WorldActor))
	{
		return false;
	}
	WorldActor->Regenerate(1337);
	constexpr int32 ChunkSize = 64;
	const FVector FirstExternalChunkCenter(
		(static_cast<double>(4 * ChunkSize) + ChunkSize * 0.5)
			* MatterFlux::PlayableLevel::TerrainCellSize,
		0.0,
		0.0);
	const double StartSeconds = FPlatformTime::Seconds();
	WorldActor->SetWorldStreamingFocus(FirstExternalChunkCenter);
	const double BoundaryMilliseconds =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	double MaximumStreamingTickMilliseconds = 0.0;
	int32 MaximumStreamingTickFrame = INDEX_NONE;
	int32 MaximumTickChunksBefore = 0;
	int32 MaximumTickChunksAfter = 0;
	int32 MaximumTickPendingBefore = 0;
	int32 MaximumTickPendingAfter = 0;
	int32 StreamingFrames = 0;
	for (;
		StreamingFrames < 320
			&& WorldActor->GetPendingProceduralPopulationUpdateCount() > 0;
		++StreamingFrames)
	{
		const int32 ChunksBefore =
			WorldActor->GetProceduralPopulationChunkCount();
		const int32 PendingBefore =
			WorldActor->GetPendingProceduralPopulationUpdateCount();
		const double TickStartSeconds = FPlatformTime::Seconds();
		WorldActor->Tick(1.0f / 60.0f);
		const double TickMilliseconds =
			(FPlatformTime::Seconds() - TickStartSeconds) * 1000.0;
		if (TickMilliseconds > MaximumStreamingTickMilliseconds)
		{
			MaximumStreamingTickMilliseconds = TickMilliseconds;
			MaximumStreamingTickFrame = StreamingFrames;
			MaximumTickChunksBefore = ChunksBefore;
			MaximumTickChunksAfter =
				WorldActor->GetProceduralPopulationChunkCount();
			MaximumTickPendingBefore = PendingBefore;
			MaximumTickPendingAfter =
				WorldActor->GetPendingProceduralPopulationUpdateCount();
		}
	}
	AddInfo(FString::Printf(
		TEXT("Infinite-population boundary plan: %.2f ms; max queued tick: %.2f ms at frame %d (chunks %d->%d, pending %d->%d); frames=%d, chunks=%d"),
		BoundaryMilliseconds,
		MaximumStreamingTickMilliseconds,
		MaximumStreamingTickFrame,
		MaximumTickChunksBefore,
		MaximumTickChunksAfter,
		MaximumTickPendingBefore,
		MaximumTickPendingAfter,
		StreamingFrames,
		WorldActor->GetProceduralPopulationChunkCount()));
	TestTrue(
		TEXT("Entering external terrain only plans work inside one 60 FPS frame"),
		BoundaryMilliseconds < 16.67);
	TestTrue(
		TEXT("Each queued population update stays inside one 60 FPS frame"),
		MaximumStreamingTickMilliseconds < 16.67);
	TestEqual(
		TEXT("The deterministic population queue drains"),
		WorldActor->GetPendingProceduralPopulationUpdateCount(),
		0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxContinuousInfiniteTerrainTraversalTest,
	"MatterFlux.Performance.ContinuousInfiniteTerrainTraversal",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::PerfFilter)

bool FMatterFluxContinuousInfiniteTerrainTraversalTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor =
		World ? World->SpawnActor<AMatterFluxPlayableWorldActor>() : nullptr;
	if (!TestNotNull(TEXT("Continuous-traversal world actor spawns"), WorldActor))
	{
		return false;
	}
	WorldActor->Regenerate(24681357);

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APlayerController* PlayerController = World->SpawnActor<APlayerController>(
		APlayerController::StaticClass(),
		FTransform::Identity,
		SpawnParameters);
	AMatterFluxCharacter* PlayerCharacter = World->SpawnActor<AMatterFluxCharacter>(
		AMatterFluxCharacter::StaticClass(),
		FTransform::Identity,
		SpawnParameters);
	if (!TestNotNull(TEXT("Traversal controller spawns"), PlayerController)
		|| !TestNotNull(TEXT("Traversal character spawns"), PlayerCharacter))
	{
		return false;
	}
	PlayerController->Possess(PlayerCharacter);

	constexpr int32 ChunkSize = 64;
	constexpr float FixedDeltaSeconds = 1.0f / 60.0f;
	constexpr float WalkingSpeed = 1200.0f;
	constexpr int32 ExternalChunkCrossings = 64;
	const float ChunkWorldSize =
		ChunkSize * MatterFlux::PlayableLevel::TerrainCellSize;
	// Start beyond the eagerly authored seed region. Keep the capsule above the
	// terrain so this test measures streaming rather than collision depenetration.
	const FVector PathStart(4.5f * ChunkWorldSize, 0.0f, 1000.0f);
	const FVector PathEnd = PathStart
		+ FVector(ExternalChunkCrossings * ChunkWorldSize, 0.0f, 0.0f);
	const int32 MovementFrames = FMath::CeilToInt(
		FVector::Distance(PathStart, PathEnd)
			/ (WalkingSpeed * FixedDeltaSeconds));
	PlayerCharacter->SetActorLocation(PathStart);

	TSharedPtr<FMatterFluxLongTraversalState> State =
		MakeShared<FMatterFluxLongTraversalState>();
	State->Test = this;
	State->World = World;
	State->WorldActor = WorldActor;
	State->Character = PlayerCharacter;
	State->PathStart = PathStart;
	State->PathEnd = PathEnd;
	State->ChunkWorldSize = ChunkWorldSize;
	State->MovementFrames = MovementFrames;
	State->PreviousChunk = FIntPoint(
		FMath::FloorToInt(PathStart.X / ChunkWorldSize),
		FMath::FloorToInt(PathStart.Y / ChunkWorldSize));
	State->PlayableWorldTickSamples.Reserve(MovementFrames);
	State->FrameIntervalSamples.Reserve(MovementFrames);
	ADD_LATENT_AUTOMATION_COMMAND(
		FMatterFluxLongTraversalLatentCommand(State));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLargeWorldStreamingPerformanceTest,
	"MatterFlux.Performance.LargeWorldStreamingMovementAndReaction",
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
	// broad-phase cost without changing reaction state or materializing Actors.
	constexpr int32 FarIgnitionQueryCount = 65536;
	const FBox FarStimulusBounds(
		FVector(10000000.0, 10000000.0, -50.0),
		FVector(10000100.0, 10000100.0, 50.0));
	int32 UnexpectedFarIgnitions = 0;
	const double FarIgnitionStart = FPlatformTime::Seconds();
	for (int32 QueryIndex = 0;
		QueryIndex < FarIgnitionQueryCount;
		++QueryIndex)
	{
		UnexpectedFarIgnitions +=
			WorldActor->ApplyMaterialStimulusToLogicalFragmentSourcesInBounds(
				FarStimulusBounds,
				FarStimulusBounds.GetCenter(),
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
			FMath::FloorToInt(
				Location.X
					/ MatterFlux::PlayableLevel::TerrainCellSize),
			FMath::FloorToInt(
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
		const int32 ResidentMaterialChunkCount =
			WorldActor->GetResidentMaterialChunkCount();
		AddInfo(FString::Printf(
			TEXT("%s resident material chunks after traversal: %d (active budget %d)"),
			Profile.Name,
			ResidentMaterialChunkCount,
			ExpectedActiveDiameter * ExpectedActiveDiameter));
		TestTrue(
			TEXT("Movement does not grow the resident simulation window"),
			ResidentMaterialChunkCount
				<= ExpectedActiveDiameter * ExpectedActiveDiameter
					+ FMath::Max(1,
						ExpectedActiveDiameter * ExpectedActiveDiameter / 3));
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
		TEXT("A nearby generated tree can be activated under load"),
		WorldActor->ApplyMaterialStimulusToFirstGeneratedTree(9091));
	double SourceActorTickSeconds = 0.0;
	double WorldActorTickSeconds = 0.0;
	const double ReactionStart = FPlatformTime::Seconds();
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
	const double ReactionMilliseconds =
		(FPlatformTime::Seconds() - ReactionStart) * 1000.0;
	AddInfo(FString::Printf(
		TEXT("LargeWorld 120 movement/material/reaction ticks: %.2f ms, ground replication=%d bytes"),
		ReactionMilliseconds,
		WorldActor->GetReplicatedTerrainMaterialByteCount()));
	AddInfo(FString::Printf(
		TEXT("LargeWorld reaction breakdown: Source Actor ticks %.2f ms, World Actor ticks %.2f ms"),
		SourceActorTickSeconds * 1000.0,
		WorldActorTickSeconds * 1000.0));
	TestTrue(
		TEXT("Active simulation remains inside the 60 FPS CPU budget"),
		ReactionMilliseconds < 2000.0);
	TestTrue(
		TEXT("Reaction adds less than 275 ms over the no-fire control ticks"),
		ReactionMilliseconds - ControlTickMilliseconds < 275.0);
	constexpr int32 GroundCellCount =
		MatterFlux::PlayableLevel::TerrainCellsX
		* MatterFlux::PlayableLevel::TerrainCellsY;
	const int32 RawGroundStateBytes =
		2 * (1 + FMath::DivideAndRoundUp(GroundCellCount, 8));
	TestTrue(
		TEXT("Localized ground fire compresses below two full bit masks"),
		WorldActor->GetReplicatedTerrainMaterialByteCount()
			< RawGroundStateBytes);
	TestTrue(
		TEXT("Reaction still creates solid output in the stress map"),
		WorldActor->GetMaterialOverrideCellCount() > 0);
	return true;
}
