#pragma once

#include "CoreMinimal.h"
#include "Fragment/FragmentTypes.h"
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

	class MATTERFLUX_API FMaskCombustion
	{
	public:
		bool Initialize(
			const FFragmentSourceMask& SourceMask,
			const FMatterFluxCombustionDefinition& Rule,
			int32 Seed);
		bool Ignite(FIntPoint Cell, FName IgnitionMaterial);
		bool ConstrainFuelMask(const TArray<uint8>& AllowedFuelMask);
		bool CaptureState(FStateSnapshot& OutState) const;
		bool RestoreState(
			const FStateSnapshot& State,
			const FMatterFluxCombustionDefinition& Rule,
			FString& OutError);
		FStepStats Step();

		bool IsInitialized() const { return bInitialized; }
		bool IsBurning() const;
		int32 CountFuelCells() const;
		int32 CountResidueCells() const;
		const TArray<uint8>& GetFuelMask() const { return FuelMask; }
		const TArray<uint8>& GetResidueMask() const { return ResidueMask; }
		const TArray<uint8>& GetBurningMask() const { return BurningMask; }
		const FMatterFluxCombustionDefinition& GetRule() const { return Rule; }

	private:
		bool IsInside(FIntPoint Cell) const;
		int32 ToIndex(FIntPoint Cell) const;
		bool PassesChance(
			FIntPoint Cell,
			int32 ChancePermille,
			uint32 Salt) const;

		FMatterFluxCombustionDefinition Rule;
		TArray<uint8> FuelMask;
		TArray<uint8> ResidueMask;
		TArray<uint8> BurningMask;
		TArray<int32> ActiveBurningIndices;
		TArray<uint32> PendingIgnitionEpochs;
		int32 Width = 0;
		int32 Height = 0;
		int32 Seed = 0;
		uint32 Tick = 0;
		uint32 PendingIgnitionEpoch = 0;
		bool bInitialized = false;
	};
}
