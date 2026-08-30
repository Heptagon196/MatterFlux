#include "Material/MatterFluxLocalMaterialReaction.h"

#include "MatterFluxContentTypes.h"

namespace
{
	constexpr uint16 MaximumMaterialIndex = 0x7fffu;
}

uint16 FLocalMaterialReactionProgram::ResolveVisibleFlameThreshold(
	const FMatterFluxContentRegistry& Registry,
	const FName CurrentMaterialId,
	const FName PrecursorHint)
{
	const FMatterFluxMaterialDefinition* Current =
		Registry.Materials.Find(CurrentMaterialId);
	if (!Current)
	{
		return 0;
	}
	if (Current->IgnitionThreshold > 0)
	{
		return Current->CombustionFlameThreshold > 0
			? Current->CombustionFlameThreshold
			: Current->IgnitionThreshold;
	}

	const auto GetPrecursorThreshold = [&Registry, CurrentMaterialId](
		const FName CandidateId)
	{
		const FMatterFluxMaterialDefinition* Candidate =
			Registry.Materials.Find(CandidateId);
		return Candidate
			&& Candidate->IgnitionThreshold > 0
			&& Candidate->CombustionProduct == CurrentMaterialId
				? (Candidate->CombustionFlameThreshold > 0
					? Candidate->CombustionFlameThreshold
					: Candidate->IgnitionThreshold)
				: static_cast<uint16>(0);
	};
	if (!PrecursorHint.IsNone())
	{
		if (const uint16 HintThreshold =
			GetPrecursorThreshold(PrecursorHint))
		{
			return HintThreshold;
		}
	}

	// World cells do not retain precursor history. Select the lowest compatible
	// threshold deterministically, so the same logical material state projects
	// identically regardless of registry/map iteration order.
	uint16 Threshold = 0;
	for (const TPair<FName, FMatterFluxMaterialDefinition>& Pair
		: Registry.Materials)
	{
		const uint16 CandidateThreshold = GetPrecursorThreshold(Pair.Key);
		if (CandidateThreshold > 0
			&& (Threshold == 0 || CandidateThreshold < Threshold))
		{
			Threshold = CandidateThreshold;
		}
	}
	return Threshold;
}

bool FLocalMaterialReactionProgram::Compile(
	const FMatterFluxContentRegistry& Registry,
	FString& OutError)
{
	OutError.Reset();
	bCompiled = false;
	MaterialIndices.Reset();
	MaterialIds.Reset();
	ThermalDefinitions.Reset();
	ContactRules.Reset();
	MaterialIds.Add(NAME_None);

	TArray<FName> SortedMaterialIds;
	Registry.Materials.GetKeys(SortedMaterialIds);
	SortedMaterialIds.Sort(FNameLexicalLess());
	for (const FName MaterialId : SortedMaterialIds)
	{
		if (MaterialId.IsNone() || MaterialIds.Num() > MaximumMaterialIndex)
		{
			OutError = FString::Printf(
				TEXT("Material '%s' cannot be assigned a local reaction index"),
				*MaterialId.ToString());
			return false;
		}
		const uint16 Index = static_cast<uint16>(MaterialIds.Add(MaterialId));
		MaterialIndices.Add(MaterialId, Index);
	}

	const auto ResolveMaterial = [this](
		const FName MaterialId,
		const bool bAllowEmpty,
		uint16& OutIndex)
	{
		if (bAllowEmpty && (MaterialId.IsNone() || MaterialId == TEXT("empty")))
		{
			OutIndex = 0;
			return true;
		}
		if (const uint16* Found = MaterialIndices.Find(MaterialId))
		{
			OutIndex = *Found;
			return true;
		}
		return false;
	};

	for (uint16 MaterialIndex = 1;
		MaterialIndex < static_cast<uint16>(MaterialIds.Num());
		++MaterialIndex)
	{
		const FName MaterialId = MaterialIds[MaterialIndex];
		const FMatterFluxMaterialDefinition& Definition =
			Registry.Materials.FindChecked(MaterialId);
		FMaterialThermalDefinition Thermal;
		Thermal.MaterialIndex = MaterialIndex;
		Thermal.DefaultEnergy = Definition.DefaultEnergy;
		Thermal.DefaultLifetime = Definition.LifetimeSteps;
		Thermal.ConductivityPermille = Definition.ConductivityPermille;
		Thermal.CoolingPerStep = Definition.CoolingPerStep;
		Thermal.IgnitionThreshold = Definition.IgnitionThreshold;
		Thermal.IgnitionProductEnergy = Definition.CombustionEnergy;
		Thermal.IgnitionEmissionAmount = Definition.CombustionEmissionAmount;
		Thermal.IgnitionSecondaryEmissionAmount =
			Definition.CombustionSecondaryEmissionAmount;
		if (!ResolveMaterial(
				Definition.CombustionProduct,
				true,
				Thermal.IgnitionProductMaterialIndex)
			|| !ResolveMaterial(
				Definition.CombustionEmissionMaterial,
				true,
				Thermal.IgnitionEmissionMaterialIndex)
			|| !ResolveMaterial(
				Definition.CombustionSecondaryEmissionMaterial,
				true,
				Thermal.IgnitionSecondaryEmissionMaterialIndex)
			|| !Thermal.IsValid())
		{
			OutError = FString::Printf(
				TEXT("Material '%s' has invalid local thermal settings"),
				*MaterialId.ToString());
			return false;
		}
		ThermalDefinitions.Add(Thermal);
	}

	TArray<FName> SortedReactionIds;
	Registry.Reactions.GetKeys(SortedReactionIds);
	SortedReactionIds.Sort(FNameLexicalLess());
	TSet<uint32> ResolvedPairs;
	for (const FName ReactionId : SortedReactionIds)
	{
		const FMatterFluxReactionDefinition& Definition =
			Registry.Reactions.FindChecked(ReactionId);
		FLocalMaterialContactRule Rule;
		Rule.RuleId = ReactionId;
		if (!ResolveMaterial(Definition.InputA, false, Rule.InputA)
			|| !ResolveMaterial(Definition.InputB, false, Rule.InputB)
			|| !ResolveMaterial(Definition.OutputA, true, Rule.OutputA)
			|| !ResolveMaterial(Definition.OutputB, true, Rule.OutputB))
		{
			OutError = FString::Printf(
				TEXT("Reaction '%s' has an unknown material reference"),
				*ReactionId.ToString());
			return false;
		}
		const uint16 Lower = FMath::Min(Rule.InputA, Rule.InputB);
		const uint16 Upper = FMath::Max(Rule.InputA, Rule.InputB);
		const uint32 PairKey = (static_cast<uint32>(Lower) << 16u) | Upper;
		if (ResolvedPairs.Contains(PairKey))
		{
			OutError = FString::Printf(
				TEXT("Reaction '%s' duplicates an unordered input pair in the local program"),
				*ReactionId.ToString());
			return false;
		}
		ResolvedPairs.Add(PairKey);
		Rule.ChancePermille = Definition.ChancePermille;
		Rule.EnergyDeltaA = Definition.EnergyDeltaA;
		Rule.EnergyDeltaB = Definition.EnergyDeltaB;
		for (const FMatterFluxReactionEmissionDefinition& Authored
			: Definition.Emissions)
		{
			uint16 EmissionMaterial = 0;
			if (!ResolveMaterial(Authored.Material, false, EmissionMaterial))
			{
				OutError = FString::Printf(
					TEXT("Reaction '%s' has an unknown emission material"),
					*ReactionId.ToString());
				return false;
			}
			Rule.Emissions.Add({
				EmissionMaterial,
				Authored.Amount,
				Authored.Energy,
				Authored.SourceSide
					== EMatterFluxReactionEmissionSourceSide::A
					? EMaterialEmissionSourceSide::A
					: EMaterialEmissionSourceSide::B });
		}
		if (!Rule.IsValid())
		{
			OutError = FString::Printf(
				TEXT("Reaction '%s' cannot compile to a local contact rule"),
				*ReactionId.ToString());
			return false;
		}
		ContactRules.Add(MoveTemp(Rule));
	}
	bCompiled = true;
	return true;
}

bool FLocalMaterialReactionProgram::TryGetMaterialIndex(
	const FName MaterialId,
	uint16& OutIndex) const
{
	OutIndex = 0;
	if (!bCompiled)
	{
		return false;
	}
	if (MaterialId.IsNone() || MaterialId == TEXT("empty"))
	{
		return true;
	}
	if (const uint16* Found = MaterialIndices.Find(MaterialId))
	{
		OutIndex = *Found;
		return true;
	}
	return false;
}

bool FLocalMaterialReactionProgram::TryGetMaterialId(
	const uint16 MaterialIndex,
	FName& OutId) const
{
	OutId = NAME_None;
	if (!bCompiled || !MaterialIds.IsValidIndex(MaterialIndex))
	{
		return false;
	}
	OutId = MaterialIds[MaterialIndex];
	return true;
}

bool FLocalMaterialReactionProgram::MakeState(
	const FName MaterialId,
	const uint16 Amount,
	const TOptional<uint16> Energy,
	FMaterialElementState& OutState) const
{
	OutState = FMaterialElementState();
	uint16 MaterialIndex = 0;
	if (Amount == 0)
	{
		return TryGetMaterialIndex(NAME_None, MaterialIndex);
	}
	if (!TryGetMaterialIndex(MaterialId, MaterialIndex) || MaterialIndex == 0)
	{
		return false;
	}
	const FMaterialThermalDefinition* Thermal = ThermalDefinitions.FindByPredicate(
		[MaterialIndex](const FMaterialThermalDefinition& Candidate)
		{
			return Candidate.MaterialIndex == MaterialIndex;
		});
	if (!Thermal)
	{
		return false;
	}
	OutState.MaterialIndex = MaterialIndex;
	OutState.Amount = Amount;
	OutState.Energy = Energy.IsSet() ? Energy.GetValue() : Thermal->DefaultEnergy;
	OutState.RemainingLifetime = Thermal->DefaultLifetime;
	return true;
}

bool FLocalMaterialReactionProgram::EvaluatePair(
	const FMaterialElementAddress& AddressA,
	const FMaterialElementState& StateA,
	const FMaterialElementAddress& AddressB,
	const FMaterialElementState& StateB,
	const int32 StoreRevision,
	const FLocalMaterialReactionContext& Context,
	FMaterialDeltaBatch& OutBatch,
	FString& OutError) const
{
	class FPairView final : public IMaterialElementView
	{
	public:
		int32 Revision = 0;
		TMap<FMaterialElementAddress, FMaterialElementState> States;
		virtual int32 GetStoreRevision() const override { return Revision; }
		virtual bool TryGetState(
			const FMaterialElementAddress& Address,
			FMaterialElementState& OutState) const override
		{
			if (const FMaterialElementState* Found = States.Find(Address))
			{
				OutState = *Found;
				return true;
			}
			return false;
		}
	};

	if (!bCompiled || StoreRevision < 0
		|| !AddressA.IsValid() || !AddressB.IsValid()
		|| AddressA == AddressB || !StateA.IsValid() || !StateB.IsValid())
	{
		OutError = TEXT("local reaction pair input is invalid");
		OutBatch = FMaterialDeltaBatch();
		return false;
	}
	FPairView View;
	View.Revision = StoreRevision;
	View.States.Add(AddressA, StateA);
	View.States.Add(AddressB, StateB);
	return FLocalMaterialReactionKernel::Evaluate(
		View,
		ThermalDefinitions,
		ContactRules,
		{ FMaterialContact(AddressA, AddressB, 1) },
		Context,
		OutBatch,
		OutError);
}
