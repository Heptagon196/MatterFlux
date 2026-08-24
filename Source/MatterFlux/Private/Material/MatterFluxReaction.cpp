#include "Material/MatterFluxReaction.h"

namespace MatterFlux::Reaction
{
	namespace
	{
		FMatterFluxReactionDefinition NormalizePropagatingRule(
			const FMatterFluxReactionDefinition& Definition)
		{
			FMatterFluxReactionDefinition Rule = Definition;
			Rule.Kind = FMatterFluxReactionDefinition::EKind::Propagating;
			if (Rule.OutputB.IsNone())
			{
				Rule.OutputB = Rule.InputB;
			}
			return Rule;
		}
	}

	bool FMaskReaction::Initialize(
		const FFragmentSourceMask& SourceMask,
		const FMatterFluxReactionDefinition& InRule,
		const int32 Seed)
	{
		FString Error;
		const FMatterFluxReactionDefinition Normalized =
			NormalizePropagatingRule(InRule);
		bInitialized = ReactionEngine.InitializeGrid(
			SourceMask, Normalized, Seed, Error);
		if (bInitialized)
		{
			Rule = Normalized;
		}
		return bInitialized;
	}

	bool FMaskReaction::Activate(
		const FIntPoint Cell,
		const FName StimulusMaterial)
	{
		return bInitialized
			&& ReactionEngine.Activate(Cell, StimulusMaterial);
	}

	bool FMaskReaction::ConstrainInputMask(
		const TArray<uint8>& AllowedInputMask)
	{
		return bInitialized
			&& ReactionEngine.ConstrainInputMask(AllowedInputMask);
	}

	bool FMaskReaction::CaptureState(FStateSnapshot& OutState) const
	{
		MatterFlux::Reaction::FGridStateSnapshot State;
		if (!bInitialized || !ReactionEngine.CaptureState(State))
		{
			return false;
		}
		OutState.RuleId = State.RuleId;
		OutState.Width = State.Width;
		OutState.Height = State.Height;
		OutState.Seed = State.Seed;
		OutState.Tick = State.Tick;
		// Keep caller-owned allocations reusable. Source streaming captures these
		// arrays every fixed step, so moving a temporary here would reintroduce
		// allocator churn even though the generic engine itself is allocation-stable.
		OutState.InputMask = State.InputMask;
		OutState.OutputMask = State.OutputMask;
		OutState.ActiveMask = State.ActiveState;
		return true;
	}

	bool FMaskReaction::RestoreState(
		const FStateSnapshot& State,
		const FMatterFluxReactionDefinition& InRule,
		FString& OutError)
	{
		MatterFlux::Reaction::FGridStateSnapshot GenericState;
		GenericState.RuleId = State.RuleId;
		GenericState.Width = State.Width;
		GenericState.Height = State.Height;
		GenericState.Seed = State.Seed;
		GenericState.Tick = State.Tick;
		GenericState.InputMask = State.InputMask;
		GenericState.OutputMask = State.OutputMask;
		GenericState.ActiveState = State.ActiveMask;
		const FMatterFluxReactionDefinition Normalized =
			NormalizePropagatingRule(InRule);
		if (!ReactionEngine.RestoreState(
			GenericState, Normalized, OutError))
		{
			return false;
		}
		Rule = Normalized;
		bInitialized = true;
		return true;
	}

	FStepStats FMaskReaction::Step(const int32 MaxNewActivations)
	{
		const MatterFlux::Reaction::FGridStepResult Generic =
			ReactionEngine.Step(MaxNewActivations);
		FStepStats Stats;
		Stats.ActivatedCells = Generic.ActivatedCells;
		Stats.ConsumedInputCells = Generic.CompletedCells;
		Stats.MaterialEmissionCells = Generic.EmissionCells;
		Stats.ChangedCellIndices = Generic.ChangedCellIndices;
		return Stats;
	}

	bool FMaskReaction::IsActive() const
	{
		return bInitialized && ReactionEngine.HasActiveCells();
	}

	int32 FMaskReaction::CountInputCells() const
	{
		int32 Count = 0;
		for (const uint8 Value : ReactionEngine.GetInputMask())
		{
			Count += Value != 0 ? 1 : 0;
		}
		return Count;
	}

	int32 FMaskReaction::CountOutputCells() const
	{
		int32 Count = 0;
		for (const uint8 Value : ReactionEngine.GetOutputMask())
		{
			Count += Value != 0 ? 1 : 0;
		}
		return Count;
	}

	const TArray<uint8>& FMaskReaction::GetInputMask() const
	{
		return bInitialized ? ReactionEngine.GetInputMask() : EmptyMask;
	}

	const TArray<uint8>& FMaskReaction::GetOutputMask() const
	{
		return bInitialized ? ReactionEngine.GetOutputMask() : EmptyMask;
	}

	const TArray<uint8>& FMaskReaction::GetActiveMask() const
	{
		return bInitialized ? ReactionEngine.GetActiveState() : EmptyMask;
	}
}
