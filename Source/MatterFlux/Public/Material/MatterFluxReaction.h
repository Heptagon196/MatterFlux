#pragma once

#include "CoreMinimal.h"
#include "Fragment/FragmentTypes.h"
#include "Material/MatterFluxMaterialReactionEngine.h"
#include "MatterFluxContentTypes.h"

namespace MatterFlux::Reaction
{
	/** Active reaction cells only use a flame presentation when fire is the
	 * authored stimulus. Corrosion remains active gameplay state, but presents
	 * through its configured material emission instead of fake flames. */
	inline bool UsesFlamePresentation(
		const FMatterFluxReactionDefinition& Rule)
	{
		return Rule.Kind
				== FMatterFluxReactionDefinition::EKind::Propagating
			&& Rule.InputB == TEXT("fire");
	}

	struct FStepStats
	{
		int32 ActivatedCells = 0;
		int32 ConsumedInputCells = 0;
		TArray<FIntPoint> MaterialEmissionCells;
		TArray<int32> ChangedCellIndices;
	};

	struct FStateSnapshot
	{
		FName RuleId = NAME_None;
		int32 Width = 0;
		int32 Height = 0;
		int32 Seed = 0;
		uint32 Tick = 0;
		TArray<uint8> InputMask;
		TArray<uint8> OutputMask;
		TArray<uint8> ActiveMask;
	};

	/**
	 * Thin mask adapter over FMaterialReactionEngine. It adds no material-
	 * specific algorithm; all transitions are defined by the supplied rule.
	 */
	class MATTERFLUX_API FMaskReaction
	{
	public:
		bool Initialize(
			const FFragmentSourceMask& SourceMask,
			const FMatterFluxReactionDefinition& Rule,
			int32 Seed);
		bool Activate(FIntPoint Cell, FName StimulusMaterial);
		bool ConstrainInputMask(const TArray<uint8>& AllowedInputMask);
		bool CaptureState(FStateSnapshot& OutState) const;
		bool RestoreState(
			const FStateSnapshot& State,
			const FMatterFluxReactionDefinition& Rule,
			FString& OutError);
		FStepStats Step(int32 MaxNewActivations = MAX_int32);

		bool IsInitialized() const { return bInitialized; }
		bool IsActive() const;
		int32 CountInputCells() const;
		int32 CountOutputCells() const;
		const TArray<uint8>& GetInputMask() const;
		const TArray<uint8>& GetOutputMask() const;
		const TArray<uint8>& GetActiveMask() const;
		const FMatterFluxReactionDefinition& GetRule() const { return Rule; }

	private:
		FMatterFluxReactionDefinition Rule;
		MatterFlux::Reaction::FMaterialReactionEngine ReactionEngine;
		TArray<uint8> EmptyMask;
		bool bInitialized = false;
	};
}
