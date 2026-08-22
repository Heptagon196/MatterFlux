#include "Material/MatterFluxCombustion.h"

namespace MatterFlux::Combustion
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

	bool FMaskCombustion::Initialize(
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

	bool FMaskCombustion::Ignite(
		const FIntPoint Cell,
		const FName IgnitionMaterial)
	{
		return bInitialized
			&& ReactionEngine.Activate(Cell, IgnitionMaterial);
	}

	bool FMaskCombustion::ConstrainFuelMask(
		const TArray<uint8>& AllowedFuelMask)
	{
		return bInitialized
			&& ReactionEngine.ConstrainInputMask(AllowedFuelMask);
	}

	bool FMaskCombustion::CaptureState(FStateSnapshot& OutState) const
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
		OutState.FuelMask = State.InputMask;
		OutState.ResidueMask = State.OutputMask;
		OutState.BurningMask = State.ActiveState;
		return true;
	}

	bool FMaskCombustion::RestoreState(
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
		GenericState.InputMask = State.FuelMask;
		GenericState.OutputMask = State.ResidueMask;
		GenericState.ActiveState = State.BurningMask;
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

	FStepStats FMaskCombustion::Step(const int32 MaxNewIgnitions)
	{
		const MatterFlux::Reaction::FGridStepResult Generic =
			ReactionEngine.Step(MaxNewIgnitions);
		FStepStats Stats;
		Stats.IgnitedCells = Generic.ActivatedCells;
		Stats.ConsumedFuelCells = Generic.CompletedCells;
		Stats.SmokeEmissionCells = Generic.EmissionCells;
		Stats.ChangedCellIndices = Generic.ChangedCellIndices;
		return Stats;
	}

	bool FMaskCombustion::IsBurning() const
	{
		return bInitialized && ReactionEngine.HasActiveCells();
	}

	int32 FMaskCombustion::CountFuelCells() const
	{
		int32 Count = 0;
		for (const uint8 Value : ReactionEngine.GetInputMask())
		{
			Count += Value != 0 ? 1 : 0;
		}
		return Count;
	}

	int32 FMaskCombustion::CountResidueCells() const
	{
		int32 Count = 0;
		for (const uint8 Value : ReactionEngine.GetOutputMask())
		{
			Count += Value != 0 ? 1 : 0;
		}
		return Count;
	}

	const TArray<uint8>& FMaskCombustion::GetFuelMask() const
	{
		return bInitialized ? ReactionEngine.GetInputMask() : EmptyMask;
	}

	const TArray<uint8>& FMaskCombustion::GetResidueMask() const
	{
		return bInitialized ? ReactionEngine.GetOutputMask() : EmptyMask;
	}

	const TArray<uint8>& FMaskCombustion::GetBurningMask() const
	{
		return bInitialized ? ReactionEngine.GetActiveState() : EmptyMask;
	}
}
