#pragma once

#include "CoreMinimal.h"
#include "Material/MatterFluxReaction.h"

namespace MatterFlux::Reaction
{
	struct MATTERFLUX_API FSourceRuntimeSettings
	{
		float StepSeconds = 0.1f;
		int32 MaxStepsPerAdvance = 3;
		/** Limits deterministic activation instead of changing an entire mask at once. */
		int32 MaxActivationsPerStep = 1;

		bool IsValid() const;
	};

	struct MATTERFLUX_API FSourceAdvanceResult
	{
		bool bStateChanged = false;
		bool bGeometryChanged = false;
		int32 Steps = 0;
		TArray<FIntPoint> MaterialEmissionCells;
		TArray<int32> ChangedCellIndices;
	};

	struct MATTERFLUX_API FSourceRuntimeSnapshot
	{
		FStateSnapshot ReactionState;
		float ReactionAccumulator = 0.0f;
		int32 TotalMaterialEmissionCount = 0;
	};

	/**
	 * Owns one source's deterministic reaction state and fixed-step debt.
	 * It deliberately has no UObject or Actor dependency, so a world-level
	 * store can simulate many logical sources and render them in shared batches.
	 */
	class MATTERFLUX_API FSourceReactionRuntime
	{
	public:
		FSourceReactionRuntime();
		~FSourceReactionRuntime();

		FSourceReactionRuntime(const FSourceReactionRuntime&) = delete;
		FSourceReactionRuntime& operator=(
			const FSourceReactionRuntime&) = delete;

		bool Initialize(
			const FSourceRuntimeSettings& Settings,
			const FFragmentSourceMask& SourceMask,
			const FMatterFluxReactionDefinition& Rule,
			int32 Seed,
			FString& OutError);
		bool RestoreState(
			const FSourceRuntimeSettings& Settings,
			const FSourceRuntimeSnapshot& State,
			const FMatterFluxReactionDefinition& Rule,
			FString& OutError);
		bool CaptureState(FSourceRuntimeSnapshot& OutState) const;
		void Reset();

		bool ActivateNearest(FIntPoint RequestedCell, FName StimulusMaterial);
		bool ConstrainInputMask(const TArray<uint8>& AllowedInputMask);
		FSourceAdvanceResult AdvanceAuthority(float DeltaSeconds);

		bool IsInitialized() const { return Simulation.IsValid(); }
		bool IsActive() const
		{
			return Simulation && Simulation->IsActive();
		}
		const TArray<uint8>& GetInputMask() const;
		const TArray<uint8>& GetOutputMask() const;
		const TArray<uint8>& GetActiveMask() const;
		const FMatterFluxReactionDefinition* GetRule() const
		{
			return Simulation ? &Simulation->GetRule() : nullptr;
		}
		int32 GetTotalMaterialEmissionCount() const
		{
			return TotalMaterialEmissionCount;
		}

	private:
		TUniquePtr<FMaskReaction> Simulation;
		FSourceRuntimeSettings RuntimeSettings;
		int32 Width = 0;
		int32 Height = 0;
		float StepAccumulator = 0.0f;
		int32 TotalMaterialEmissionCount = 0;
	};
}
