#pragma once

#include "CoreMinimal.h"
#include "Material/MatterFluxReaction.h"
#include "Material/MatterFluxGroundStateChunk.h"

namespace MatterFlux::Reaction
{
	struct MATTERFLUX_API FGroundRuntimeSettings
	{
		int32 Width = 0;
		int32 Height = 0;
		int32 ChunkSize = 64;
		float StepSeconds = 0.1f;
		int32 MaxStepsPerAdvance = 3;

		bool IsValid() const;
	};

	struct MATTERFLUX_API FGroundAdvanceResult
	{
		bool bStateChanged = false;
		int32 Steps = 0;
		TArray<int32> ChangedCellIndices;
	};

	struct MATTERFLUX_API FGroundRuntimeSnapshot
	{
		FStateSnapshot ReactionState;
		float StepAccumulator = 0.0f;
		int32 Revision = 0;
	};

	enum class EGroundChunkApplyResult : uint8
	{
		NoChange,
		Applied,
		Rejected
	};

	/**
	 * Owns deterministic ground-reaction stepping and its chunk replication
	 * transaction. UE Actors only transport the completed chunk payloads.
	 */
	class MATTERFLUX_API FGroundReactionRuntime
	{
	public:
		FGroundReactionRuntime();
		~FGroundReactionRuntime();

		FGroundReactionRuntime(const FGroundReactionRuntime&) = delete;
		FGroundReactionRuntime& operator=(
			const FGroundReactionRuntime&) = delete;

		bool Initialize(
			const FGroundRuntimeSettings& Settings,
			const FFragmentSourceMask& GroundMask,
			const FMatterFluxReactionDefinition& Rule,
			int32 Seed,
			FString& OutError);
		void Reset();
		bool IsInitialized() const { return Simulation.IsValid(); }

		bool Activate(FIntPoint Cell, FName StimulusMaterial);
		bool ActivateNearestInput(
			FIntPoint RequestedCell,
			FName StimulusMaterial,
			int32 MaximumSearchRadius,
			FIntPoint& OutActivatedCell);
		FGroundAdvanceResult AdvanceAuthority(float DeltaSeconds);
		bool BuildInitialReplication(
			TArray<FMatterFluxGroundStateChunk>& OutChunks,
			FString& OutError) const;
		bool BuildPendingReplication(
			TArray<FMatterFluxGroundStateChunk>& OutChunks,
			FString& OutError);
		EGroundChunkApplyResult ApplyReplicatedChunk(
			const FMatterFluxGroundStateChunk& State,
			FString& OutError);
		bool CaptureState(FGroundRuntimeSnapshot& OutState) const;
		bool RestoreState(
			const FGroundRuntimeSettings& Settings,
			const FGroundRuntimeSnapshot& State,
			const FMatterFluxReactionDefinition& Rule,
			FString& OutError);

		bool HasPendingReplication() const
		{
			return !DirtyChunks.IsEmpty();
		}
		bool IsActive() const
		{
			return Simulation && Simulation->IsActive();
		}
		int32 CountOutputCells() const;
		void GatherActiveCellIndices(TArray<int32>& OutCellIndices) const;
		void GatherOutputCellIndices(TArray<int32>& OutCellIndices) const;
		void GatherActiveChunkCoordinates(
			TArray<FIntPoint>& OutChunkCoordinates) const;
		void GatherVisibleCellIndicesForChunks(
			TConstArrayView<FIntPoint> ChunkCoordinates,
			TArray<int32>& OutOutputCellIndices,
			TArray<int32>& OutActiveCellIndices) const;
		int32 GetRevision() const { return Revision; }
		const TArray<uint8>& GetOutputMask() const { return VisibleOutputMask; }
		const TArray<uint8>& GetActiveMask() const { return VisibleActiveMask; }
		const FMatterFluxReactionDefinition* GetRule() const
		{
			return Simulation ? &Simulation->GetRule() : nullptr;
		}

	private:
		bool BuildReplicationForCoordinates(
			TConstArrayView<FIntPoint> Coordinates,
			int32 TargetRevision,
			TArray<FMatterFluxGroundStateChunk>& OutChunks,
			FString& OutError) const;
		void MarkCellDirty(int32 CellIndex);
		void SetActiveCellIndexState(int32 CellIndex, bool bActive);
		void SetOutputCellIndexState(int32 CellIndex, bool bOutput);
		void RefreshVisibleCellIndicesForChunk(FIntPoint ChunkCoordinate);
		void RebuildVisibleCellIndices();

		TUniquePtr<FMaskReaction> Simulation;
		FGroundRuntimeSettings RuntimeSettings;
		TArray<uint8> VisibleOutputMask;
		TArray<uint8> VisibleActiveMask;
		TMap<FIntPoint, TSet<int32>> ActiveCellIndicesByChunk;
		TMap<FIntPoint, TSet<int32>> OutputCellIndicesByChunk;
		TSet<FIntPoint> DirtyChunks;
		TMap<FIntPoint, int32> AppliedChunkRevisions;
		float StepAccumulator = 0.0f;
		int32 Revision = 0;
	};
}
