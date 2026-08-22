#pragma once

#include "CoreMinimal.h"
#include "Fragment/FragmentTypes.h"
#include "Material/MatterFluxMaterialReactionEngine.h"
#include "MatterFluxContentTypes.h"

namespace MatterFlux::Combustion
{
	struct FStepStats
	{
		int32 IgnitedCells = 0;
		int32 ConsumedFuelCells = 0;
		TArray<FIntPoint> SmokeEmissionCells;
		TArray<int32> ChangedCellIndices;
	};

	struct FStateSnapshot
	{
		FName RuleId = NAME_None;
		int32 Width = 0;
		int32 Height = 0;
		int32 Seed = 0;
		uint32 Tick = 0;
		TArray<uint8> FuelMask;
		TArray<uint8> ResidueMask;
		TArray<uint8> BurningMask;
	};

	/**
	 * Fire-facing compatibility adapter. It contains no combustion algorithm:
	 * all state transitions are delegated to FMaterialReactionEngine. The old
	 * names remain only because save/network payloads and VFX still describe
	 * this particular presentation as combustion.
	 */
	class MATTERFLUX_API FMaskCombustion
	{
	public:
		bool Initialize(
			const FFragmentSourceMask& SourceMask,
			const FMatterFluxReactionDefinition& Rule,
			int32 Seed);
		bool Ignite(FIntPoint Cell, FName IgnitionMaterial);
		bool ConstrainFuelMask(const TArray<uint8>& AllowedFuelMask);
		bool CaptureState(FStateSnapshot& OutState) const;
		bool RestoreState(
			const FStateSnapshot& State,
			const FMatterFluxReactionDefinition& Rule,
			FString& OutError);
		FStepStats Step(int32 MaxNewIgnitions = MAX_int32);

		bool IsInitialized() const { return bInitialized; }
		bool IsBurning() const;
		int32 CountFuelCells() const;
		int32 CountResidueCells() const;
		const TArray<uint8>& GetFuelMask() const;
		const TArray<uint8>& GetResidueMask() const;
		const TArray<uint8>& GetBurningMask() const;
		const FMatterFluxReactionDefinition& GetRule() const { return Rule; }

	private:
		FMatterFluxReactionDefinition Rule;
		MatterFlux::Reaction::FMaterialReactionEngine ReactionEngine;
		TArray<uint8> EmptyMask;
		bool bInitialized = false;
	};
}
