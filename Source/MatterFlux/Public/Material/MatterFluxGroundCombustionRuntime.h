#pragma once

#include "CoreMinimal.h"
#include "Material/MatterFluxCombustion.h"
#include "Material/MatterFluxGroundStateChunk.h"

namespace MatterFlux::Combustion
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
		FStateSnapshot CombustionState;
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
	 * Owns deterministic ground-combustion stepping and its chunk replication
	 * transaction. UE Actors only transport the completed chunk payloads.
	 */
	class MATTERFLUX_API FGroundCombustionRuntime
	{
	public:
		FGroundCombustionRuntime();
		~FGroundCombustionRuntime();

		FGroundCombustionRuntime(const FGroundCombustionRuntime&) = delete;
		FGroundCombustionRuntime& operator=(
			const FGroundCombustionRuntime&) = delete;

		bool Initialize(
			const FGroundRuntimeSettings& Settings,
			const FFragmentSourceMask& GroundMask,
			const FMatterFluxCombustionDefinition& Rule,
			int32 Seed,
			FString& OutError);
		void Reset();
		bool IsInitialized() const { return Simulation.IsValid(); }

		bool Ignite(FIntPoint Cell, FName IgnitionMaterial);
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
			const FMatterFluxCombustionDefinition& Rule,
			FString& OutError);

		bool HasPendingReplication() const
		{
			return !DirtyChunks.IsEmpty();
		}
		bool IsBurning() const
		{
			return Simulation && Simulation->IsBurning();
		}
		int32 CountResidueCells() const;
		void GatherBurningCellIndices(TArray<int32>& OutCellIndices) const;
		void GatherResidueCellIndices(TArray<int32>& OutCellIndices) const;
		void GatherBurningChunkCoordinates(
			TArray<FIntPoint>& OutChunkCoordinates) const;
		void GatherVisibleCellIndicesForChunks(
			TConstArrayView<FIntPoint> ChunkCoordinates,
			TArray<int32>& OutResidueCellIndices,
			TArray<int32>& OutBurningCellIndices) const;
		int32 GetRevision() const { return Revision; }
		const TArray<uint8>& GetResidueMask() const { return VisibleResidueMask; }
		const TArray<uint8>& GetBurningMask() const { return VisibleBurningMask; }
		const FMatterFluxCombustionDefinition* GetRule() const
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
		void SetBurningCellIndexState(int32 CellIndex, bool bBurning);
		void SetResidueCellIndexState(int32 CellIndex, bool bResidue);
		void RefreshVisibleCellIndicesForChunk(FIntPoint ChunkCoordinate);
		void RebuildVisibleCellIndices();

		TUniquePtr<FMaskCombustion> Simulation;
		FGroundRuntimeSettings RuntimeSettings;
		TArray<uint8> VisibleResidueMask;
		TArray<uint8> VisibleBurningMask;
		TMap<FIntPoint, TSet<int32>> BurningCellIndicesByChunk;
		TMap<FIntPoint, TSet<int32>> ResidueCellIndicesByChunk;
		TSet<FIntPoint> DirtyChunks;
		TMap<FIntPoint, int32> AppliedChunkRevisions;
		float StepAccumulator = 0.0f;
		int32 Revision = 0;
	};
}
