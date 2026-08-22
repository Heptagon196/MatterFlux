#include "Material/MatterFluxMaterialReactionEngine.h"

namespace MatterFlux::Reaction
{
	namespace
	{
		constexpr int64 MaximumReactionCellCount = 1024ll * 1024ll;

		uint32 MixBits(uint32 Value)
		{
			Value ^= Value >> 16u;
			Value *= 0x7feb352du;
			Value ^= Value >> 15u;
			Value *= 0x846ca68bu;
			Value ^= Value >> 16u;
			return Value;
		}

		uint32 StableRuleIdHash(const FName RuleId)
		{
			// FName's runtime index is process-local. Hash the authored ID so a
			// listen host, late client and another platform make the same roll.
			uint32 Hash = 2166136261u;
			const FString Text = RuleId.ToString().ToLower();
			for (const TCHAR Character : Text)
			{
				Hash ^= static_cast<uint32>(Character);
				Hash *= 16777619u;
			}
			return Hash;
		}

		bool IsPermille(const int32 Value)
		{
			return Value >= 0 && Value <= 1000;
		}

		bool IsValidContactRule(const FMatterFluxReactionDefinition& Rule)
		{
			return Rule.Kind == FMatterFluxReactionDefinition::EKind::Contact
				&& !Rule.Id.IsNone()
				&& !Rule.InputA.IsNone()
				&& !Rule.InputB.IsNone()
				&& !Rule.OutputA.IsNone()
				&& !Rule.OutputB.IsNone()
				&& IsPermille(Rule.ChancePermille);
		}

		bool IsValidPropagatingRule(const FMatterFluxReactionDefinition& Rule)
		{
			const bool bHasEmission = !Rule.EmissionMaterial.IsNone()
				&& Rule.EmissionMaterial != TEXT("empty");
			return Rule.Kind == FMatterFluxReactionDefinition::EKind::Propagating
				&& !Rule.Id.IsNone()
				&& !Rule.InputA.IsNone()
				&& !Rule.InputB.IsNone()
				&& !Rule.OutputA.IsNone()
				&& (bHasEmission || Rule.EmissionChancePermille == 0)
				&& IsPermille(Rule.ChancePermille)
				&& IsPermille(Rule.PropagationChancePermille)
				&& IsPermille(Rule.EmissionChancePermille)
				&& Rule.DurationSteps >= 1
				&& Rule.DurationSteps <= 255;
		}
	}

	const FMatterFluxReactionDefinition*
	FMaterialReactionEngine::FindPropagatingRule(
		const FMatterFluxContentRegistry& Registry,
		const FName InputMaterial,
		const FName StimulusMaterial)
	{
		TArray<FName> RuleIds;
		Registry.Reactions.GetKeys(RuleIds);
		RuleIds.Sort(FNameLexicalLess());
		for (const FName RuleId : RuleIds)
		{
			const FMatterFluxReactionDefinition* Rule =
				Registry.Reactions.Find(RuleId);
			if (Rule
				&& Rule->Kind
					== FMatterFluxReactionDefinition::EKind::Propagating
				&& Rule->InputA == InputMaterial
				&& (StimulusMaterial.IsNone()
					|| Rule->InputB == StimulusMaterial))
			{
				return Rule;
			}
		}
		return nullptr;
	}

	bool FMaterialReactionEngine::EvaluateContact(
		const FMatterFluxReactionDefinition& Rule,
		const FName FirstMaterial,
		const FName SecondMaterial,
		const FDeterministicContext& Context,
		FContactResult& OutResult)
	{
		OutResult.FirstMaterial = FirstMaterial;
		OutResult.SecondMaterial = SecondMaterial;
		OutResult.bReacted = false;
		if (!IsValidContactRule(Rule))
		{
			return false;
		}

		const bool bForward = FirstMaterial == Rule.InputA
			&& SecondMaterial == Rule.InputB;
		const bool bReverse = FirstMaterial == Rule.InputB
			&& SecondMaterial == Rule.InputA;
		if (!bForward && !bReverse)
		{
			return true;
		}

		const uint32 Roll = MixBits(
			static_cast<uint32>(Context.Seed)
				^ Context.Tick * 0x9e3779b9u
				^ static_cast<uint32>(Context.FirstCell.X) * 0x85ebca6bu
				^ static_cast<uint32>(Context.FirstCell.Y) * 0xc2b2ae35u
				^ static_cast<uint32>(Context.SecondCell.X) * 0x27d4eb2fu
				^ static_cast<uint32>(Context.SecondCell.Y) * 0x165667b1u
				^ StableRuleIdHash(Rule.Id));
		if (static_cast<int32>(Roll % 1000u) >= Rule.ChancePermille)
		{
			return true;
		}

		OutResult.FirstMaterial = bForward ? Rule.OutputA : Rule.OutputB;
		OutResult.SecondMaterial = bForward ? Rule.OutputB : Rule.OutputA;
		OutResult.bReacted = true;
		return true;
	}

	bool FMaterialReactionEngine::InitializeGrid(
		const FFragmentSourceMask& SourceMask,
		const FMatterFluxReactionDefinition& InRule,
		const int32 InSeed,
		FString& OutError)
	{
		OutError.Reset();
		bInitialized = false;
		InputMask.Reset();
		OutputMask.Reset();
		ActiveState.Reset();
		ActiveIndices.Reset();
		PendingActivationEpochs.Reset();
		Width = 0;
		Height = 0;
		Tick = 0;
		PendingActivationEpoch = 0;

		const int64 CellCount = static_cast<int64>(SourceMask.Width)
			* static_cast<int64>(SourceMask.Height);
		if (!IsValidPropagatingRule(InRule)
			|| SourceMask.Width <= 0
			|| SourceMask.Height <= 0
			|| CellCount <= 0
			|| CellCount > MaximumReactionCellCount
			|| SourceMask.SolidMask.Num() != CellCount)
		{
			OutError = TEXT("Reaction grid dimensions or rule are invalid");
			return false;
		}
		bool bHasInput = false;
		for (const uint8 Value : SourceMask.SolidMask)
		{
			if (Value > 1)
			{
				OutError = TEXT("Reaction grid mask must be binary");
				return false;
			}
			bHasInput |= Value != 0;
		}
		if (!bHasInput)
		{
			OutError = TEXT("Reaction grid must contain at least one input cell");
			return false;
		}

		Rule = InRule;
		Width = SourceMask.Width;
		Height = SourceMask.Height;
		Seed = InSeed;
		InputMask = SourceMask.SolidMask;
		OutputMask.Init(0, InputMask.Num());
		ActiveState.Init(0, InputMask.Num());
		PendingActivationEpochs.Init(0, InputMask.Num());
		bInitialized = true;
		return true;
	}

	bool FMaterialReactionEngine::Activate(
		const FIntPoint Cell,
		const FName StimulusMaterial)
	{
		if (!bInitialized || StimulusMaterial != Rule.InputB || !IsInside(Cell))
		{
			return false;
		}
		const int32 Index = ToIndex(Cell);
		if (InputMask[Index] == 0 || ActiveState[Index] != 0
			|| !PassesChance(Cell, Rule.ChancePermille, 0x41435456u))
		{
			return false;
		}
		ActiveState[Index] = static_cast<uint8>(Rule.DurationSteps);
		ActiveIndices.Add(Index);
		return true;
	}

	bool FMaterialReactionEngine::ConstrainInputMask(
		const TArray<uint8>& AllowedInputMask)
	{
		if (!bInitialized || AllowedInputMask.Num() != InputMask.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < InputMask.Num(); ++Index)
		{
			if (AllowedInputMask[Index] == 0)
			{
				InputMask[Index] = 0;
				OutputMask[Index] = 0;
				ActiveState[Index] = 0;
			}
		}
		ActiveIndices.RemoveAll([this](const int32 Index)
		{
			return !ActiveState.IsValidIndex(Index) || ActiveState[Index] == 0;
		});
		return true;
	}

	FGridStepResult FMaterialReactionEngine::Step(const int32 MaxNewActivations)
	{
		FGridStepResult Result;
		if (!bInitialized || ActiveIndices.IsEmpty())
		{
			return Result;
		}
		++Tick;
		TArray<int32> Current = ActiveIndices;
		Current.Sort();
		static const FIntPoint NeighborOffsets[] = {
			FIntPoint(0, 1), FIntPoint(-1, 0),
			FIntPoint(1, 0), FIntPoint(0, -1)
		};
		TArray<int32> Pending;
		++PendingActivationEpoch;
		if (PendingActivationEpoch == 0)
		{
			PendingActivationEpochs.Init(0, InputMask.Num());
			PendingActivationEpoch = 1;
		}

		for (const int32 ActiveIndex : Current)
		{
			const FIntPoint Cell(ActiveIndex % Width, ActiveIndex / Width);
			if (!Rule.EmissionMaterial.IsNone()
				&& Rule.EmissionMaterial != TEXT("empty")
				&& PassesChance(
					Cell, Rule.EmissionChancePermille, 0x454d4954u))
			{
				Result.EmissionCells.Add(Cell);
			}
			for (const FIntPoint Offset : NeighborOffsets)
			{
				if (Pending.Num() >= MaxNewActivations)
				{
					break;
				}
				const FIntPoint Neighbor = Cell + Offset;
				if (!IsInside(Neighbor))
				{
					continue;
				}
				const int32 NeighborIndex = ToIndex(Neighbor);
				if (InputMask[NeighborIndex] == 0
					|| ActiveState[NeighborIndex] != 0
					|| PendingActivationEpochs[NeighborIndex] == PendingActivationEpoch
					|| !PassesChance(Neighbor, Rule.PropagationChancePermille,
						0x50524f50u ^ static_cast<uint32>(ActiveIndex)))
				{
					continue;
				}
				Pending.Add(NeighborIndex);
				PendingActivationEpochs[NeighborIndex] = PendingActivationEpoch;
			}
		}

		TArray<int32> Next;
		Next.Reserve(Current.Num() + Pending.Num());
		for (const int32 Index : Current)
		{
			Result.ChangedCellIndices.Add(Index);
			if (ActiveState[Index] > 0)
			{
				--ActiveState[Index];
			}
			if (ActiveState[Index] == 0 && InputMask[Index] != 0)
			{
				InputMask[Index] = 0;
				OutputMask[Index] = 1;
				++Result.CompletedCells;
			}
			else if (ActiveState[Index] != 0)
			{
				Next.Add(Index);
			}
		}
		for (const int32 Index : Pending)
		{
			if (InputMask[Index] != 0 && ActiveState[Index] == 0)
			{
				ActiveState[Index] = static_cast<uint8>(Rule.DurationSteps);
				Next.Add(Index);
				++Result.ActivatedCells;
				Result.ChangedCellIndices.Add(Index);
			}
		}
		Next.Sort();
		ActiveIndices = MoveTemp(Next);
		return Result;
	}

	bool FMaterialReactionEngine::CaptureState(FGridStateSnapshot& OutState) const
	{
		if (!bInitialized)
		{
			return false;
		}
		OutState.RuleId = Rule.Id;
		OutState.Width = Width;
		OutState.Height = Height;
		OutState.Seed = Seed;
		OutState.Tick = Tick;
		OutState.InputMask = InputMask;
		OutState.OutputMask = OutputMask;
		OutState.ActiveState = ActiveState;
		return true;
	}

	bool FMaterialReactionEngine::RestoreState(
		const FGridStateSnapshot& State,
		const FMatterFluxReactionDefinition& InRule,
		FString& OutError)
	{
		OutError.Reset();
		const int64 CellCount = static_cast<int64>(State.Width) * State.Height;
		if (!IsValidPropagatingRule(InRule) || State.RuleId != InRule.Id
			|| State.Width <= 0 || State.Height <= 0 || CellCount <= 0
			|| CellCount > MaximumReactionCellCount
			|| State.InputMask.Num() != CellCount
			|| State.OutputMask.Num() != CellCount
			|| State.ActiveState.Num() != CellCount)
		{
			OutError = TEXT("Reaction snapshot dimensions, rule, or arrays are invalid");
			return false;
		}
		TArray<int32> RestoredActive;
		for (int32 Index = 0; Index < State.InputMask.Num(); ++Index)
		{
			const uint8 Input = State.InputMask[Index];
			const uint8 Output = State.OutputMask[Index];
			const uint8 Active = State.ActiveState[Index];
			if (Input > 1 || Output > 1 || (Input != 0 && Output != 0)
				|| Active > InRule.DurationSteps || (Active != 0 && Input == 0))
			{
				OutError = FString::Printf(TEXT("Reaction snapshot cell %d is inconsistent"), Index);
				return false;
			}
			if (Active != 0)
			{
				RestoredActive.Add(Index);
			}
		}
		Rule = InRule;
		Width = State.Width;
		Height = State.Height;
		Seed = State.Seed;
		Tick = State.Tick;
		InputMask = State.InputMask;
		OutputMask = State.OutputMask;
		ActiveState = State.ActiveState;
		ActiveIndices = MoveTemp(RestoredActive);
		PendingActivationEpochs.Init(0, InputMask.Num());
		PendingActivationEpoch = 0;
		bInitialized = true;
		return true;
	}

	bool FMaterialReactionEngine::IsInside(const FIntPoint Cell) const
	{
		return Cell.X >= 0 && Cell.X < Width && Cell.Y >= 0 && Cell.Y < Height;
	}

	int32 FMaterialReactionEngine::ToIndex(const FIntPoint Cell) const
	{
		return Cell.Y * Width + Cell.X;
	}

	bool FMaterialReactionEngine::PassesChance(
		const FIntPoint Cell,
		const int32 ChancePermille,
		const uint32 Salt) const
	{
		if (ChancePermille >= 1000)
		{
			return true;
		}
		if (ChancePermille <= 0)
		{
			return false;
		}
		const uint32 Hash = MixBits(static_cast<uint32>(Seed)
			^ static_cast<uint32>(Cell.X) * 0x9e3779b9u
			^ static_cast<uint32>(Cell.Y) * 0x85ebca6bu
			^ Tick * 0xc2b2ae35u ^ Salt ^ StableRuleIdHash(Rule.Id));
		return static_cast<int32>(Hash % 1000u) < ChancePermille;
	}
}
