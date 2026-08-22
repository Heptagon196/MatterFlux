#pragma once

#include "CoreMinimal.h"
#include "Material/MatterFluxMaterialWorld.h"
#include "Material/MatterFluxReplicatedMaterialState.h"

namespace MatterFlux::Material
{
	/** Configuration owned by the material simulation runtime seam. */
	struct MATTERFLUX_API FRuntimeSettings
	{
		FWorldSettings World;
		float StepSeconds = 0.05f;
		int32 MaxStepsPerAdvance = 4;

		bool IsValid() const;
	};

	/** Observable outcome of one authority-frame advance. */
	struct MATTERFLUX_API FRuntimeAdvanceResult
	{
		bool bFocusChanged = false;
		bool bStateChanged = false;
		int32 Steps = 0;
		int32 LogicalStep = 0;
	};

	enum class EReplicatedStateApplyResult : uint8
	{
		NoChange,
		Applied,
		Rejected
	};

	/**
	 * Owns fixed-step scheduling and focus transitions for a chunked material
	 * world. A focus-changing frame preserves time debt but deliberately does
	 * not stack simulation work on top of chunk archive/restore work.
	 */
	class MATTERFLUX_API FSimulationRuntime
	{
	public:
		FSimulationRuntime();
		~FSimulationRuntime();

		FSimulationRuntime(const FSimulationRuntime&) = delete;
		FSimulationRuntime& operator=(const FSimulationRuntime&) = delete;

		bool Initialize(
			const FRuntimeSettings& Settings,
			const FMatterFluxContentRegistry& Registry,
			int32 Seed,
			TConstArrayView<FIntPoint> InitialFocuses,
			FString& OutError);
		void Reset();
		bool IsInitialized() const { return MaterialWorld.IsValid(); }

		FRuntimeAdvanceResult AdvanceAuthority(
			float DeltaSeconds,
			TConstArrayView<FIntPoint> Focuses);
		bool BuildReplicatedState(
			int32 MapSeed,
			int32 PreviousRevision,
			FMatterFluxReplicatedMaterialState& OutState,
			FString& OutError);
		EReplicatedStateApplyResult ApplyReplicatedState(
			int32 ExpectedMapSeed,
			const FMatterFluxReplicatedMaterialState& State,
			FString& OutError);
		bool ExportActiveState(TArray<uint8>& OutState, FString& OutError) const;
		bool ImportActiveState(
			const TArray<uint8>& State,
			int32& OutLogicalStep,
			FIntPoint& OutPrimaryFocus,
			FString& OutError);
		bool SeedSurface(const TArray<FSeedCell>& SeedCells);
		int32 DisplaceLiquids(
			TConstArrayView<FIntPoint> OccupiedCells,
			int32 MaxSearchRadius = 64);
		int32 DisplaceLiquids(
			TConstArrayView<FLiquidDisplacementConstraint> Constraints,
			int32 MaxSearchRadius = 64);
		void SetFocuses(TConstArrayView<FIntPoint> Focuses);
		void RequireFocusReconciliation() { CurrentFocuses.Reset(); }

		bool SetCell(const FIntPoint& WorldCell, FName MaterialId);
		FName GetMaterialAt(const FIntPoint& WorldCell) const;
		bool TryGetCellSnapshot(
			const FIntPoint& WorldCell,
			FCellSnapshot& OutSnapshot) const;
		int32 CountMaterial(FName MaterialId) const;
		int64 SumMaterialAmount(FName MaterialId) const;
		int32 GetResidentChunkCount() const;
		int32 GetArchivedChunkCount() const;
		int32 GetSimulationFocusCount() const;
		void GetActiveCells(TArray<FCellSnapshot>& OutCells) const;
		void GetAllCells(TArray<FCellSnapshot>& OutCells) const;
		void ConsumeProjectionDirtyChunks(TArray<FIntPoint>& OutChunks);
		int32 GetLogicalStep() const { return LogicalStep; }
		int32 GetAppliedStateRevision() const { return AppliedStateRevision; }
		bool NeedsReplicationPublish() const { return bReplicationDirty; }
		const TArray<FIntPoint>& GetFocuses() const { return CurrentFocuses; }

	private:
		static bool NormalizeFocuses(
			TConstArrayView<FIntPoint> Focuses,
			TArray<FIntPoint>& OutFocuses);

		TUniquePtr<FChunkedMaterialWorld> MaterialWorld;
		FRuntimeSettings RuntimeSettings;
		TArray<FIntPoint> CurrentFocuses;
		float StepAccumulator = 0.0f;
		int32 LogicalStep = 0;
		int32 AppliedStateRevision = INDEX_NONE;
		int32 RejectedStateRevision = INDEX_NONE;
		bool bReplicationDirty = false;
	};
}
