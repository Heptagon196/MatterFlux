#pragma once

#include "CoreMinimal.h"
#include "Material/MatterFluxCombustion.h"

namespace MatterFlux::Combustion
{
	struct MATTERFLUX_API FSourceRuntimeSettings
	{
		float StepSeconds = 0.1f;
		int32 MaxStepsPerAdvance = 3;

		bool IsValid() const;
	};

	struct MATTERFLUX_API FSourceAdvanceResult
	{
		bool bStateChanged = false;
		bool bGeometryChanged = false;
		int32 Steps = 0;
		TArray<FIntPoint> SmokeEmissionCells;
		TArray<int32> ChangedCellIndices;
	};

	struct MATTERFLUX_API FSourceRuntimeSnapshot
	{
		FStateSnapshot CombustionState;
		float CombustionAccumulator = 0.0f;
		int32 TotalSmokeEmissionCount = 0;
	};

	/**
	 * Owns one source's deterministic combustion state and fixed-step debt.
	 * It deliberately has no UObject or Actor dependency, so a world-level
	 * store can simulate many logical sources and render them in shared batches.
	 */
	class MATTERFLUX_API FSourceCombustionRuntime
	{
	public:
		FSourceCombustionRuntime();
		~FSourceCombustionRuntime();

		FSourceCombustionRuntime(const FSourceCombustionRuntime&) = delete;
		FSourceCombustionRuntime& operator=(
			const FSourceCombustionRuntime&) = delete;

		bool Initialize(
			const FSourceRuntimeSettings& Settings,
			const FFragmentSourceMask& SourceMask,
			const FMatterFluxCombustionDefinition& Rule,
			int32 Seed,
			FString& OutError);
		bool RestoreState(
			const FSourceRuntimeSettings& Settings,
			const FSourceRuntimeSnapshot& State,
			const FMatterFluxCombustionDefinition& Rule,
			FString& OutError);
		bool CaptureState(FSourceRuntimeSnapshot& OutState) const;
		void Reset();

		bool IgniteNearest(FIntPoint RequestedCell, FName IgnitionMaterial);
		bool ConstrainFuelMask(const TArray<uint8>& AllowedFuelMask);
		FSourceAdvanceResult AdvanceAuthority(float DeltaSeconds);

		bool IsInitialized() const { return Simulation.IsValid(); }
		bool IsBurning() const
		{
			return Simulation && Simulation->IsBurning();
		}
		const TArray<uint8>& GetFuelMask() const;
		const TArray<uint8>& GetResidueMask() const;
		const TArray<uint8>& GetBurningMask() const;
		const FMatterFluxCombustionDefinition* GetRule() const
		{
			return Simulation ? &Simulation->GetRule() : nullptr;
		}
		int32 GetTotalSmokeEmissionCount() const
		{
			return TotalSmokeEmissionCount;
		}

	private:
		TUniquePtr<FMaskCombustion> Simulation;
		FSourceRuntimeSettings RuntimeSettings;
		int32 Width = 0;
		int32 Height = 0;
		float StepAccumulator = 0.0f;
		int32 TotalSmokeEmissionCount = 0;
	};
}
