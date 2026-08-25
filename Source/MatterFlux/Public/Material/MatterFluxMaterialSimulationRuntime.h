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

	/**
	 * Canonical moving material fact. Surface cells and airborne particles are
	 * two states of the same conserved material; renderers and spell actors only
	 * project this record and never own its payload.
	 */
	struct MATTERFLUX_API FAirborneParticle
	{
		FGuid BatchId;
		FName MaterialId = NAME_None;
		FVector WorldPosition = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		float Radius = 2.0f;
		float GravityScale = 0.0f;
		float RemainingLifetime = 1.0f;
		int32 CellCount = 1;
		int32 ConservedMaterialAmount = 0;
		int32 EventSeed = 0;
		int32 ParticleIndex = INDEX_NONE;
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
			TConstArrayView<FIntPoint> Focuses,
			int32 MaxStepsThisAdvance = MAX_int32);
		/**
		 * Predicts whether the next authority advance has fixed-step debt to
		 * consume. Callers use this to keep unrelated streaming commits off the
		 * same game-thread frame; a focus reconciliation may still defer the step.
		 */
		bool WillAdvanceStep(float DeltaSeconds) const;
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
		bool SeedSurface(
			const TArray<FSeedCell>& SeedCells,
			bool bFinalizeBaseline);
		void WakeSurfaceCells(TConstArrayView<FIntPoint> WorldCells);
		int32 DisplaceLiquids(
			TConstArrayView<FIntPoint> OccupiedCells,
			int32 MaxSearchRadius = 64);
		int32 DisplaceLiquids(
			TConstArrayView<FLiquidDisplacementConstraint> Constraints,
			int32 MaxSearchRadius = 64);
		int32 DisplacePowders(
			TConstArrayView<FLiquidDisplacementConstraint> Constraints,
			int32 MaxSearchRadius = 64);
		void SetFocuses(TConstArrayView<FIntPoint> Focuses);
		void RequireFocusReconciliation() { CurrentFocuses.Reset(); }

		FGuid SpawnAirborneParticles(
			FName MaterialId,
			TConstArrayView<FVector> WorldPositions,
			TConstArrayView<FVector> InitialVelocities,
			int32 CellCount,
			int32 ConservedAmountPerCell,
			float Radius,
			float GravityScale,
			float Lifetime,
			int32 EventSeed);
		/**
		 * Advances canonical particles through the geometry adapter. Returning true
		 * means the callback atomically transferred that particle into another
		 * canonical material state and it can be removed from the airborne set.
		 */
		int32 AdvanceAirborneParticles(
			float DeltaSeconds,
			TFunctionRef<bool(FAirborneParticle&, float)> AdvanceParticle);
		void GetAirborneParticlesForBatch(
			const FGuid& BatchId,
			TArray<FAirborneParticle>& OutParticles) const;
		bool GetAirborneParticleBounds(
			FBox& OutBounds,
			float& OutMaximumRadius,
			float& OutMaximumSpeed,
			float& OutMaximumGravityScale) const;
		bool HasAirborneParticleBatch(const FGuid& BatchId) const;
		int32 CountAirborneParticles(FName MaterialId = NAME_None) const;
		int64 SumAirborneMaterialAmount(FName MaterialId) const;
		/** Removes every canonical airborne particle accepted by the predicate. */
		int64 RemoveAirborneParticles(
			TFunctionRef<bool(const FAirborneParticle&)> ShouldRemove);

		bool SetCell(const FIntPoint& WorldCell, FName MaterialId);
		bool SetCellAmount(
			const FIntPoint& WorldCell,
			FName MaterialId,
			uint16 Amount);
		int32 AddCellAmount(
			const FIntPoint& WorldCell,
			FName MaterialId,
			uint16 Amount);
		int32 AddPowderAmountAtStableSurface(
			const FIntPoint& ImpactCell,
			FName MaterialId,
			uint16 Amount,
			int32 MaximumTravelCells,
			FIntPoint& OutDestinationCell);
		bool SetExternalSupportHeight(
			const FIntPoint& WorldCell,
			int32 Height);
		bool ClearExternalSupportHeight(const FIntPoint& WorldCell);
		FName GetMaterialAt(const FIntPoint& WorldCell) const;
		uint16 GetMaterialAmountAt(
			const FIntPoint& WorldCell,
			FName MaterialId) const;
		bool TryGetCellSnapshot(
			const FIntPoint& WorldCell,
			FCellSnapshot& OutSnapshot) const;
		int32 CountMaterial(FName MaterialId) const;
		int64 SumMaterialAmount(FName MaterialId) const;
		int32 GetResidentChunkCount() const;
		int32 GetArchivedChunkCount() const;
		int32 GetSimulationFocusCount() const;
		void GetActiveCells(TArray<FCellSnapshot>& OutCells) const;
		void GetResidentCells(TArray<FCellSnapshot>& OutCells) const;
		void GetCellsInChunks(
			TConstArrayView<FIntPoint> Chunks,
			TArray<FCellSnapshot>& OutCells) const;
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
		TArray<FAirborneParticle> AirborneParticles;
		float StepAccumulator = 0.0f;
		int32 LogicalStep = 0;
		int32 AppliedStateRevision = INDEX_NONE;
		int32 RejectedStateRevision = INDEX_NONE;
		uint32 NextAirborneBatchSerial = 1;
		bool bReplicationDirty = false;
	};
}
