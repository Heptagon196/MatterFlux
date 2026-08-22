#pragma once

#include "CoreMinimal.h"
#include "Fragment/FragmentTypes.h"
#include "MatterFluxContentTypes.h"

namespace MatterFlux::Reaction
{
	/** Inputs that make probability rolls stable across machines and save/load. */
	struct FDeterministicContext
	{
		int32 Seed = 0;
		uint32 Tick = 0;
		FIntPoint FirstCell = FIntPoint::ZeroValue;
		FIntPoint SecondCell = FIntPoint::ZeroValue;
	};

	struct FContactResult
	{
		FName FirstMaterial = NAME_None;
		FName SecondMaterial = NAME_None;
		bool bReacted = false;
	};

	struct FGridStepResult
	{
		int32 ActivatedCells = 0;
		int32 CompletedCells = 0;
		TArray<FIntPoint> EmissionCells;
		TArray<int32> ChangedCellIndices;
	};

	struct FGridStateSnapshot
	{
		FName RuleId = NAME_None;
		int32 Width = 0;
		int32 Height = 0;
		int32 Seed = 0;
		uint32 Tick = 0;
		TArray<uint8> InputMask;
		TArray<uint8> OutputMask;
		TArray<uint8> ActiveState;
	};

	/**
	 * Deterministic native reaction engine. Lua only creates definitions; it is
	 * never invoked while cells are advancing.
	 */
	class MATTERFLUX_API FMaterialReactionEngine
	{
	public:
		/**
		 * Finds the lexically first matching propagating rule. Centralizing this
		 * lookup prevents TMap iteration order from changing gameplay results.
		 */
		static const FMatterFluxReactionDefinition* FindPropagatingRule(
			const FMatterFluxContentRegistry& Registry,
			FName InputMaterial,
			FName StimulusMaterial = NAME_None);

		static bool EvaluateContact(
			const FMatterFluxReactionDefinition& Rule,
			FName FirstMaterial,
			FName SecondMaterial,
			const FDeterministicContext& Context,
			FContactResult& OutResult);

		bool InitializeGrid(
			const FFragmentSourceMask& SourceMask,
			const FMatterFluxReactionDefinition& Rule,
			int32 Seed,
			FString& OutError);
		bool Activate(FIntPoint Cell, FName StimulusMaterial);
		bool ConstrainInputMask(const TArray<uint8>& AllowedInputMask);
		FGridStepResult Step(int32 MaxNewActivations = MAX_int32);
		bool CaptureState(FGridStateSnapshot& OutState) const;
		bool RestoreState(
			const FGridStateSnapshot& State,
			const FMatterFluxReactionDefinition& Rule,
			FString& OutError);

		bool IsInitialized() const { return bInitialized; }
		bool HasActiveCells() const { return !ActiveIndices.IsEmpty(); }
		const TArray<uint8>& GetInputMask() const { return InputMask; }
		const TArray<uint8>& GetOutputMask() const { return OutputMask; }
		const TArray<uint8>& GetActiveState() const { return ActiveState; }
		const FMatterFluxReactionDefinition& GetRule() const { return Rule; }

	private:
		bool IsInside(FIntPoint Cell) const;
		int32 ToIndex(FIntPoint Cell) const;
		bool PassesChance(FIntPoint Cell, int32 ChancePermille, uint32 Salt) const;

		FMatterFluxReactionDefinition Rule;
		TArray<uint8> InputMask;
		TArray<uint8> OutputMask;
		TArray<uint8> ActiveState;
		TArray<int32> ActiveIndices;
		TArray<uint32> PendingActivationEpochs;
		int32 Width = 0;
		int32 Height = 0;
		int32 Seed = 0;
		uint32 Tick = 0;
		uint32 PendingActivationEpoch = 0;
		bool bInitialized = false;
	};
}
