#include "Material/MatterFluxLocalMaterialReaction.h"

namespace MatterFluxLocalReactionPrivate
{
	constexpr uint64 FnvOffset = 14695981039346656037ull;
	constexpr uint64 FnvPrime = 1099511628211ull;

	void HashByte(uint64& Hash, const uint8 Value)
	{
		Hash ^= Value;
		Hash *= FnvPrime;
	}

	template <typename IntegerType>
	void HashInteger(uint64& Hash, const IntegerType Value)
	{
		using UnsignedType = std::make_unsigned_t<IntegerType>;
		const UnsignedType Bits = static_cast<UnsignedType>(Value);
		for (uint32 Index = 0; Index < sizeof(IntegerType); ++Index)
		{
			HashByte(Hash, static_cast<uint8>(Bits >> (Index * 8)));
		}
	}

	void HashGuid(uint64& Hash, const FGuid& Guid)
	{
		HashInteger(Hash, Guid.A);
		HashInteger(Hash, Guid.B);
		HashInteger(Hash, Guid.C);
		HashInteger(Hash, Guid.D);
	}

	void HashAddress(uint64& Hash, const FMaterialElementAddress& Address)
	{
		HashInteger(Hash, static_cast<uint8>(Address.Kind));
		HashGuid(Hash, Address.OwnerId);
		HashInteger(Hash, Address.Cell.X);
		HashInteger(Hash, Address.Cell.Y);
		HashInteger(Hash, Address.Cell.Z);
	}

	bool GuidLess(const FGuid& Left, const FGuid& Right)
	{
		if (Left.A != Right.A) return Left.A < Right.A;
		if (Left.B != Right.B) return Left.B < Right.B;
		if (Left.C != Right.C) return Left.C < Right.C;
		return Left.D < Right.D;
	}

	uint64 ContactSortKey(const FMaterialContact& Contact)
	{
		uint64 Hash = FnvOffset;
		HashAddress(Hash, Contact.ElementA);
		HashAddress(Hash, Contact.ElementB);
		HashInteger(Hash, Contact.ContactUnits);
		return Hash;
	}

	bool ContactLess(const FMaterialContact& Left, const FMaterialContact& Right)
	{
		if (!(Left.ElementA == Right.ElementA))
		{
			return MaterialElementAddressLess(Left.ElementA, Right.ElementA);
		}
		if (!(Left.ElementB == Right.ElementB))
		{
			return MaterialElementAddressLess(Left.ElementB, Right.ElementB);
		}
		return Left.ContactUnits < Right.ContactUnits;
	}

	int64 GreatestCommonDivisor(int64 Left, int64 Right)
	{
		while (Right != 0)
		{
			const int64 Remainder = Left % Right;
			Left = Right;
			Right = Remainder;
		}
		return Left;
	}

	int64 LeastCommonMultiple(const int64 Left, const int64 Right)
	{
		return Left / GreatestCommonDivisor(Left, Right) * Right;
	}

	const FMaterialThermalDefinition* FindThermal(
		const TMap<uint16, FMaterialThermalDefinition>& Materials,
		const uint16 MaterialIndex)
	{
		return Materials.Find(MaterialIndex);
	}

	void ApplySpecificEnergyDelta(
		FMaterialElementState& State,
		const int32 Delta,
		int64& InOutExplicitEnergyDelta)
	{
		if (State.Amount == 0 || Delta == 0) return;
		const uint16 Before = State.Energy;
		State.Energy = static_cast<uint16>(FMath::Clamp<int32>(
			static_cast<int32>(State.Energy) + Delta,
			0,
			MAX_uint16));
		InOutExplicitEnergyDelta += static_cast<int64>(State.Amount)
			* (static_cast<int32>(State.Energy) - Before);
	}

	FGuid MakeParticleId(
		const FMaterialElementAddress& Emitter,
		const FName RuleId,
		const FLocalMaterialReactionContext& Context,
		const uint16 Ordinal)
	{
		uint64 First = FnvOffset;
		HashAddress(First, Emitter);
		HashInteger(First, Context.Seed);
		HashInteger(First, Context.LogicalStep);
		HashInteger(First, Ordinal);
		const FTCHARToUTF8 Utf8(*RuleId.ToString());
		for (int32 Index = 0; Index < Utf8.Length(); ++Index)
		{
			HashByte(First, static_cast<uint8>(Utf8.Get()[Index]));
		}
		uint64 Second = First;
		HashInteger(Second, 0x9e3779b97f4a7c15ull);
		FGuid Result(
			static_cast<uint32>(First),
			static_cast<uint32>(First >> 32),
			static_cast<uint32>(Second),
			static_cast<uint32>(Second >> 32));
		if (!Result.IsValid()) Result.D = 1;
		return Result;
	}

	bool ProbabilityPasses(
		const FMaterialContact& Contact,
		const FLocalMaterialContactRule& Rule,
		const FLocalMaterialReactionContext& Context)
	{
		if (Rule.ChancePermille >= 1000) return true;
		if (Rule.ChancePermille <= 0) return false;
		uint64 Hash = Contact.DeterministicSortKey;
		HashInteger(Hash, Context.Seed);
		HashInteger(Hash, Context.LogicalStep);
		const FTCHARToUTF8 Utf8(*Rule.RuleId.ToString());
		for (int32 Index = 0; Index < Utf8.Length(); ++Index)
		{
			HashByte(Hash, static_cast<uint8>(Utf8.Get()[Index]));
		}
		return static_cast<int32>(Hash % 1000ull) < Rule.ChancePermille;
	}
}

FMaterialElementAddress FMaterialElementAddress::MakeWorldCell(
	const FIntVector& WorldCell)
{
	FMaterialElementAddress Result;
	Result.Kind = EMaterialElementAddressKind::WorldCell;
	Result.Cell = WorldCell;
	return Result;
}

FMaterialElementAddress FMaterialElementAddress::MakeVolumeCell(
	const FGuid& InstanceId,
	const FIntVector& LocalCell)
{
	FMaterialElementAddress Result;
	Result.Kind = EMaterialElementAddressKind::VolumeCell;
	Result.OwnerId = InstanceId;
	Result.Cell = LocalCell;
	return Result;
}

FMaterialElementAddress FMaterialElementAddress::MakeAirborneParticle(
	const FGuid& ParticleId)
{
	FMaterialElementAddress Result;
	Result.Kind = EMaterialElementAddressKind::AirborneParticle;
	Result.OwnerId = ParticleId;
	return Result;
}

bool FMaterialElementAddress::IsValid() const
{
	if (Kind == EMaterialElementAddressKind::WorldCell)
	{
		return !OwnerId.IsValid();
	}
	return OwnerId.IsValid();
}

uint32 GetTypeHash(const FMaterialElementAddress& Address)
{
	return HashCombine(
		HashCombine(GetTypeHash(static_cast<uint8>(Address.Kind)), GetTypeHash(Address.OwnerId)),
		GetTypeHash(Address.Cell));
}

bool MaterialElementAddressLess(
	const FMaterialElementAddress& Left,
	const FMaterialElementAddress& Right)
{
	using namespace MatterFluxLocalReactionPrivate;
	if (Left.Kind != Right.Kind)
	{
		return static_cast<uint8>(Left.Kind) < static_cast<uint8>(Right.Kind);
	}
	if (Left.OwnerId != Right.OwnerId) return GuidLess(Left.OwnerId, Right.OwnerId);
	if (Left.Cell.X != Right.Cell.X) return Left.Cell.X < Right.Cell.X;
	if (Left.Cell.Y != Right.Cell.Y) return Left.Cell.Y < Right.Cell.Y;
	return Left.Cell.Z < Right.Cell.Z;
}

bool FMaterialElementState::IsValid() const
{
	return Amount == 0
		? MaterialIndex == 0 && Energy == 0 && RemainingLifetime == 0
		: MaterialIndex != 0;
}

FMaterialContact::FMaterialContact(
	const FMaterialElementAddress& InElementA,
	const FMaterialElementAddress& InElementB,
	const uint16 InContactUnits)
	: ElementA(InElementA)
	, ElementB(InElementB)
	, ContactUnits(InContactUnits)
{
	if (MaterialElementAddressLess(ElementB, ElementA)) Swap(ElementA, ElementB);
	DeterministicSortKey = MatterFluxLocalReactionPrivate::ContactSortKey(*this);
}

bool FMaterialContact::IsValid() const
{
	return ElementA.IsValid() && ElementB.IsValid()
		&& !(ElementA == ElementB) && ContactUnits != 0;
}

bool FMaterialThermalDefinition::IsValid() const
{
	return MaterialIndex != 0 && ConductivityPermille <= 1000
		&& (IgnitionThreshold == 0 || IgnitionProductMaterialIndex != 0)
		&& (IgnitionEmissionAmount == 0 || IgnitionEmissionMaterialIndex != 0)
		&& (IgnitionSecondaryEmissionAmount == 0
			|| IgnitionSecondaryEmissionMaterialIndex != 0);
}

bool FLocalMaterialContactRule::IsValid() const
{
	if (RuleId.IsNone() || InputA == 0 || InputB == 0
		|| ChancePermille < 0 || ChancePermille > 1000
		|| Emissions.Num() > 2)
	{
		return false;
	}
	for (const FMaterialEmissionDefinition& Emission : Emissions)
	{
		if (!Emission.IsValid()) return false;
	}
	return true;
}

int64 FMaterialDeltaBatch::ComputeExpectedAmountBefore() const
{
	int64 Result = 0;
	for (const FMaterialElementDelta& Delta : ElementDeltas) Result += Delta.ExpectedBefore.Amount;
	return Result;
}

int64 FMaterialDeltaBatch::ComputeExpectedAmountAfterIncludingEmissions() const
{
	int64 Result = 0;
	for (const FMaterialElementDelta& Delta : ElementDeltas) Result += Delta.After.Amount;
	for (const FMaterialParticleEmission& Emission : ParticleEmissions) Result += Emission.Amount;
	return Result;
}

int64 FMaterialDeltaBatch::ComputeExpectedAmountAfterIncludingExplicitSources() const
{
	return ComputeExpectedAmountAfterIncludingEmissions() - ExplicitAmountDelta;
}

int64 FMaterialDeltaBatch::ComputeExpectedEnergyBefore() const
{
	int64 Result = 0;
	for (const FMaterialElementDelta& Delta : ElementDeltas)
	{
		Result += Delta.ExpectedBefore.GetTotalEnergy();
	}
	return Result;
}

int64 FMaterialDeltaBatch::ComputeExpectedEnergyAfterIncludingExplicitSources() const
{
	int64 Result = 0;
	for (const FMaterialElementDelta& Delta : ElementDeltas) Result += Delta.After.GetTotalEnergy();
	for (const FMaterialParticleEmission& Emission : ParticleEmissions)
	{
		Result += static_cast<int64>(Emission.Amount) * Emission.Energy;
	}
	return Result - ExplicitEnergyDelta;
}

uint64 FMaterialDeltaBatch::ComputeDeterministicHash() const
{
	using namespace MatterFluxLocalReactionPrivate;
	uint64 Hash = FnvOffset;
	HashInteger(Hash, BaseStoreRevision);
	HashInteger(Hash, TargetStoreRevision);
	HashInteger(Hash, ExplicitAmountDelta);
	HashInteger(Hash, ExplicitEnergyDelta);
	for (const FMaterialElementDelta& Delta : ElementDeltas)
	{
		HashAddress(Hash, Delta.Address);
		for (const FMaterialElementState State : { Delta.ExpectedBefore, Delta.After })
		{
			HashInteger(Hash, State.MaterialIndex);
			HashInteger(Hash, State.Amount);
			HashInteger(Hash, State.Energy);
			HashInteger(Hash, State.RemainingLifetime);
		}
	}
	for (const FMaterialParticleEmission& Emission : ParticleEmissions)
	{
		HashGuid(Hash, Emission.ParticleId);
		HashAddress(Hash, Emission.Emitter);
		HashInteger(Hash, Emission.MaterialIndex);
		HashInteger(Hash, Emission.Amount);
		HashInteger(Hash, Emission.Energy);
		HashInteger(Hash, Emission.EmissionOrdinal);
	}
	return Hash;
}

bool FMaterialDeltaBatch::IsValid(FString& OutError) const
{
	OutError.Reset();
	if (BaseStoreRevision < 0 || TargetStoreRevision < BaseStoreRevision
		|| TargetStoreRevision > BaseStoreRevision + 1)
	{
		OutError = TEXT("material batch has invalid store revisions");
		return false;
	}
	FMaterialElementAddress Previous;
	bool bHasPrevious = false;
	for (const FMaterialElementDelta& Delta : ElementDeltas)
	{
		if (!Delta.Address.IsValid() || !Delta.ExpectedBefore.IsValid() || !Delta.After.IsValid()
			|| (bHasPrevious && !MaterialElementAddressLess(Previous, Delta.Address)))
		{
			OutError = TEXT("material batch contains invalid or unsorted element deltas");
			return false;
		}
		Previous = Delta.Address;
		bHasPrevious = true;
	}
	const int64 AmountBefore = ComputeExpectedAmountBefore();
	const int64 AmountAfter = ComputeExpectedAmountAfterIncludingExplicitSources();
	if (AmountBefore != AmountAfter)
	{
		OutError = FString::Printf(
			TEXT("material batch does not conserve amount (%lld != %lld)"),
			AmountBefore, AmountAfter);
		return false;
	}
	const int64 EnergyBefore = ComputeExpectedEnergyBefore();
	const int64 EnergyAfter = ComputeExpectedEnergyAfterIncludingExplicitSources();
	if (EnergyBefore != EnergyAfter)
	{
		OutError = FString::Printf(
			TEXT("material batch energy accounting is inconsistent (%lld != %lld; explicit=%lld)"),
			EnergyBefore, EnergyAfter, ExplicitEnergyDelta);
		return false;
	}
	return true;
}

bool FMaterialElementStore::TryGetState(
	const FMaterialElementAddress& Address,
	FMaterialElementState& OutState) const
{
	if (const FMaterialElementState* Found = States.Find(Address))
	{
		OutState = *Found;
		return true;
	}
	return false;
}

bool FMaterialElementStore::SetInitialState(
	const FMaterialElementAddress& Address,
	const FMaterialElementState& State)
{
	if (!Address.IsValid() || !State.IsValid()) return false;
	States.Add(Address, State);
	++StoreRevision;
	return true;
}

bool FMaterialElementStore::ApplyBatch(
	const FMaterialDeltaBatch& Batch,
	TArray<FMaterialParticleEmission>& OutEmissions,
	FString& OutError)
{
	OutEmissions.Reset();
	if (!Batch.IsValid(OutError)) return false;
	if (Batch.BaseStoreRevision != StoreRevision)
	{
		OutError = TEXT("material batch base revision is stale");
		return false;
	}
	TMap<FMaterialElementAddress, FMaterialElementState> Candidate = States;
	for (const FMaterialElementDelta& Delta : Batch.ElementDeltas)
	{
		const FMaterialElementState* Current = Candidate.Find(Delta.Address);
		if (!Current || !(*Current == Delta.ExpectedBefore))
		{
			OutError = TEXT("material batch expected state does not match");
			return false;
		}
		Candidate.Add(Delta.Address, Delta.After);
	}
	States = MoveTemp(Candidate);
	StoreRevision = Batch.TargetStoreRevision;
	OutEmissions = Batch.ParticleEmissions;
	return true;
}

uint64 FMaterialElementStore::ComputeDeterministicHash() const
{
	using namespace MatterFluxLocalReactionPrivate;
	uint64 Hash = FnvOffset;
	HashInteger(Hash, StoreRevision);
	TArray<FMaterialElementAddress> Addresses;
	States.GetKeys(Addresses);
	Addresses.Sort(MaterialElementAddressLess);
	for (const FMaterialElementAddress& Address : Addresses)
	{
		HashAddress(Hash, Address);
		const FMaterialElementState& State = States.FindChecked(Address);
		HashInteger(Hash, State.MaterialIndex);
		HashInteger(Hash, State.Amount);
		HashInteger(Hash, State.Energy);
		HashInteger(Hash, State.RemainingLifetime);
	}
	return Hash;
}

bool FLocalMaterialReactionKernel::Evaluate(
	const IMaterialElementView& View,
	const TConstArrayView<FMaterialThermalDefinition> InMaterials,
	const TConstArrayView<FLocalMaterialContactRule> InRules,
	const TConstArrayView<FMaterialContact> InContacts,
	const FLocalMaterialReactionContext& Context,
	FMaterialDeltaBatch& OutBatch,
	FString& OutError)
{
	using namespace MatterFluxLocalReactionPrivate;
	OutBatch = FMaterialDeltaBatch();
	OutError.Reset();
	if (Context.LogicalStep < 0 || Context.MaxContacts < 0
		|| Context.MaxElementDeltas < 0 || Context.MaxEmissions < 0
		|| InContacts.Num() > Context.MaxContacts)
	{
		OutError = TEXT("local reaction context or contact budget is invalid");
		return false;
	}

	TMap<uint16, FMaterialThermalDefinition> Materials;
	for (const FMaterialThermalDefinition& Material : InMaterials)
	{
		if (!Material.IsValid() || Materials.Contains(Material.MaterialIndex))
		{
			OutError = TEXT("local reaction material definitions are invalid or duplicated");
			return false;
		}
		Materials.Add(Material.MaterialIndex, Material);
	}
	TArray<FLocalMaterialContactRule> Rules;
	Rules.Append(InRules);
	for (const FLocalMaterialContactRule& Rule : Rules)
	{
		if (!Rule.IsValid())
		{
			OutError = TEXT("local contact rule is invalid");
			return false;
		}
	}
	Rules.Sort([](const FLocalMaterialContactRule& Left, const FLocalMaterialContactRule& Right)
	{
		return Left.RuleId.LexicalLess(Right.RuleId);
	});

	TArray<FMaterialContact> Contacts;
	Contacts.Append(InContacts);
	for (FMaterialContact& Contact : Contacts)
	{
		if (!Contact.IsValid())
		{
			OutError = TEXT("local material contact is invalid");
			return false;
		}
		Contact.DeterministicSortKey = ContactSortKey(Contact);
	}
	Contacts.Sort(ContactLess);
	TArray<FMaterialContact> CombinedContacts;
	for (const FMaterialContact& Contact : Contacts)
	{
		if (!CombinedContacts.IsEmpty()
			&& CombinedContacts.Last().ElementA == Contact.ElementA
			&& CombinedContacts.Last().ElementB == Contact.ElementB)
		{
			CombinedContacts.Last().ContactUnits = static_cast<uint16>(FMath::Min<int32>(
				MAX_uint16,
				CombinedContacts.Last().ContactUnits + Contact.ContactUnits));
			CombinedContacts.Last().DeterministicSortKey =
				ContactSortKey(CombinedContacts.Last());
		}
		else CombinedContacts.Add(Contact);
	}

	TMap<FMaterialElementAddress, FMaterialElementState> Before;
	TMap<FMaterialElementAddress, FMaterialElementState> Working;
	auto Load = [&](const FMaterialElementAddress& Address) -> FMaterialElementState*
	{
		if (FMaterialElementState* Existing = Working.Find(Address)) return Existing;
		FMaterialElementState State;
		if (!View.TryGetState(Address, State) || !State.IsValid()) return nullptr;
		Before.Add(Address, State);
		Working.Add(Address, State);
		return Working.Find(Address);
	};
	TArray<FMaterialElementAddress> OrderedCoolingElements =
		Context.CoolingElements;
	OrderedCoolingElements.Sort(MaterialElementAddressLess);
	for (const FMaterialElementAddress& Address : OrderedCoolingElements)
	{
		if (!Address.IsValid() || !Load(Address))
		{
			OutError = TEXT("cooling references a missing material element");
			return false;
		}
	}

	uint16 EmissionOrdinal = 0;
	const auto ApplyIgnition = [
		&Materials,
		&Context,
		&OutBatch,
		&OutError,
		&EmissionOrdinal](
		const FMaterialElementAddress& Address,
		FMaterialElementState& State)
	{
		if (State.Amount == 0)
		{
			return true;
		}
		const FMaterialThermalDefinition* Thermal =
			FindThermal(Materials, State.MaterialIndex);
		if (!Thermal)
		{
			OutError = TEXT("reaction output has no thermal definition");
			return false;
		}
		if (Thermal->IgnitionThreshold == 0
			|| State.Energy < Thermal->IgnitionThreshold)
		{
			return true;
		}
		State.MaterialIndex = Thermal->IgnitionProductMaterialIndex;
		State.RemainingLifetime = Materials.FindChecked(
			Thermal->IgnitionProductMaterialIndex).DefaultLifetime;
		if (Thermal->IgnitionProductEnergy > 0)
		{
			ApplySpecificEnergyDelta(
				State,
				static_cast<int32>(Thermal->IgnitionProductEnergy)
					- static_cast<int32>(State.Energy),
				OutBatch.ExplicitEnergyDelta);
		}
		const auto AddIgnitionEmission = [
			&Address,
			&State,
			&Context,
			&OutBatch,
			&OutError,
			&EmissionOrdinal](
			const uint16 MaterialIndex,
			const uint16 Amount)
		{
			if (Amount == 0)
			{
				return true;
			}
			FMaterialParticleEmission& Emission =
				OutBatch.ParticleEmissions.AddDefaulted_GetRef();
			Emission.Emitter = Address;
			Emission.MaterialIndex = MaterialIndex;
			Emission.Amount = Amount;
			Emission.Energy = State.Energy;
			Emission.EmissionOrdinal = EmissionOrdinal++;
			Emission.ParticleId = MakeParticleId(
				Address,
				TEXT("thermal.ignition"),
				Context,
				Emission.EmissionOrdinal);
			// Combustion exhaust is an authored source term, not material carved
			// out of an otherwise full topology cell.
			OutBatch.ExplicitAmountDelta += Emission.Amount;
			OutBatch.ExplicitEnergyDelta +=
				static_cast<int64>(Emission.Amount) * Emission.Energy;
			if (OutBatch.ParticleEmissions.Num() > Context.MaxEmissions)
			{
				OutError = TEXT("local reaction emission budget exceeded");
				return false;
			}
			return true;
		};
		if (!AddIgnitionEmission(
				Thermal->IgnitionEmissionMaterialIndex,
				Thermal->IgnitionEmissionAmount)
			|| !AddIgnitionEmission(
				Thermal->IgnitionSecondaryEmissionMaterialIndex,
				Thermal->IgnitionSecondaryEmissionAmount))
		{
			return false;
		}
		return true;
	};
	for (const FMaterialContact& Contact : CombinedContacts)
	{
		if (!Load(Contact.ElementA) || !Load(Contact.ElementB))
		{
			OutError = TEXT("material contact references a missing element");
			return false;
		}
		// Loading B may grow the TMap and invalidate the pointer returned while
		// loading A. Resolve both pointers only after all insertions for this pair.
		FMaterialElementState* StateA = Working.Find(Contact.ElementA);
		FMaterialElementState* StateB = Working.Find(Contact.ElementB);
		check(StateA && StateB);
		const FMaterialThermalDefinition* ThermalA = FindThermal(Materials, StateA->MaterialIndex);
		const FMaterialThermalDefinition* ThermalB = FindThermal(Materials, StateB->MaterialIndex);
		if (!ThermalA || !ThermalB)
		{
			OutError = TEXT("material element has no thermal definition");
			return false;
		}

		const uint16 Conductivity = FMath::Min(
			ThermalA->ConductivityPermille,
			ThermalB->ConductivityPermille);
		if (Conductivity > 0 && StateA->Energy != StateB->Energy
			&& StateA->Amount > 0 && StateB->Amount > 0)
		{
			FMaterialElementState* Donor = StateA->Energy > StateB->Energy ? StateA : StateB;
			FMaterialElementState* Receiver = Donor == StateA ? StateB : StateA;
			const int64 Difference = static_cast<int64>(Donor->Energy) - Receiver->Energy;
			const int64 MinimumThermalAmount = FMath::Min<int64>(
				Donor->Amount, Receiver->Amount);
			const int64 RequestedTransfer =
				Difference * Contact.ContactUnits * Conductivity
					* MinimumThermalAmount / 1000;
			// Never step past the common equilibrium temperature. This cap also
			// makes high conductivity converge instead of swapping hot/cold states.
			const int64 EqualizingTransfer =
				Difference * Donor->Amount * Receiver->Amount
					/ (static_cast<int64>(Donor->Amount) + Receiver->Amount);
			int64 Transfer = FMath::Min(RequestedTransfer, EqualizingTransfer);
			Transfer = FMath::Min(Transfer, Donor->GetTotalEnergy());
			Transfer = FMath::Min(
				Transfer,
				static_cast<int64>(MAX_uint16 - Receiver->Energy) * Receiver->Amount);
			const int64 Quantum = LeastCommonMultiple(Donor->Amount, Receiver->Amount);
			Transfer -= Transfer % Quantum;
			if (Transfer > 0)
			{
				Donor->Energy = static_cast<uint16>(
					(Donor->GetTotalEnergy() - Transfer) / Donor->Amount);
				Receiver->Energy = static_cast<uint16>(
					(Receiver->GetTotalEnergy() + Transfer) / Receiver->Amount);
			}
		}

		const FLocalMaterialContactRule* Matched = nullptr;
		bool bElementAIsRuleA = true;
		for (const FLocalMaterialContactRule& Rule : Rules)
		{
			if (StateA->MaterialIndex == Rule.InputA && StateB->MaterialIndex == Rule.InputB)
			{
				Matched = &Rule;
				bElementAIsRuleA = true;
				break;
			}
			if (StateA->MaterialIndex == Rule.InputB && StateB->MaterialIndex == Rule.InputA)
			{
				Matched = &Rule;
				bElementAIsRuleA = false;
				break;
			}
		}
		if (!Matched || !ProbabilityPasses(Contact, *Matched, Context))
		{
			if (!ApplyIgnition(Contact.ElementA, *StateA)
				|| !ApplyIgnition(Contact.ElementB, *StateB))
			{
				return false;
			}
			continue;
		}

		FMaterialElementState* RuleAState = bElementAIsRuleA ? StateA : StateB;
		FMaterialElementState* RuleBState = bElementAIsRuleA ? StateB : StateA;
		const FMaterialElementAddress& RuleAAddress =
			bElementAIsRuleA ? Contact.ElementA : Contact.ElementB;
		const FMaterialElementAddress& RuleBAddress =
			bElementAIsRuleA ? Contact.ElementB : Contact.ElementA;
		ApplySpecificEnergyDelta(*RuleAState, Matched->EnergyDeltaA, OutBatch.ExplicitEnergyDelta);
		ApplySpecificEnergyDelta(*RuleBState, Matched->EnergyDeltaB, OutBatch.ExplicitEnergyDelta);

		for (const FMaterialEmissionDefinition& Definition : Matched->Emissions)
		{
			FMaterialElementState* Source = Definition.SourceSide == EMaterialEmissionSourceSide::A
				? RuleAState : RuleBState;
			const FMaterialElementAddress& SourceAddress =
				Definition.SourceSide == EMaterialEmissionSourceSide::A
				? RuleAAddress : RuleBAddress;
			if (Source->Amount < Definition.Amount)
			{
				OutError = TEXT("reaction emission exceeds source material amount");
				return false;
			}
			const int64 RemovedEnergy = static_cast<int64>(Definition.Amount) * Source->Energy;
			Source->Amount -= Definition.Amount;
			if (Source->Amount == 0) *Source = FMaterialElementState();
			FMaterialParticleEmission& Emission = OutBatch.ParticleEmissions.AddDefaulted_GetRef();
			Emission.Emitter = SourceAddress;
			Emission.MaterialIndex = Definition.MaterialIndex;
			Emission.Amount = Definition.Amount;
			Emission.Energy = Definition.Energy;
			Emission.EmissionOrdinal = EmissionOrdinal++;
			Emission.ParticleId = MakeParticleId(
				SourceAddress, Matched->RuleId, Context, Emission.EmissionOrdinal);
			OutBatch.ExplicitEnergyDelta +=
				static_cast<int64>(Emission.Amount) * Emission.Energy - RemovedEnergy;
			if (OutBatch.ParticleEmissions.Num() > Context.MaxEmissions)
			{
				OutError = TEXT("local reaction emission budget exceeded");
				return false;
			}
		}

		const auto ApplyOutput = [&OutBatch, &Materials](
			FMaterialElementState& State, const uint16 OutputMaterial)
		{
			if (OutputMaterial != 0)
			{
				State.MaterialIndex = OutputMaterial;
				State.RemainingLifetime =
					Materials.FindChecked(OutputMaterial).DefaultLifetime;
				return;
			}
			OutBatch.ExplicitAmountDelta -= State.Amount;
			OutBatch.ExplicitEnergyDelta -= State.GetTotalEnergy();
			State = FMaterialElementState();
		};
		ApplyOutput(*RuleAState, Matched->OutputA);
		ApplyOutput(*RuleBState, Matched->OutputB);
		if (!ApplyIgnition(Contact.ElementA, *StateA)
			|| !ApplyIgnition(Contact.ElementB, *StateB))
		{
			return false;
		}
	}

	TArray<FMaterialElementAddress> Touched;
	Working.GetKeys(Touched);
	Touched.Sort(MaterialElementAddressLess);
	for (const FMaterialElementAddress& Address : Touched)
	{
		FMaterialElementState& State = Working.FindChecked(Address);
		if (State.Amount == 0) continue;
		const FMaterialThermalDefinition* Thermal = FindThermal(Materials, State.MaterialIndex);
		if (!Thermal)
		{
			OutError = TEXT("reaction output has no thermal definition");
			return false;
		}
		if (Context.bApplyCooling
			&& Thermal->CoolingPerStep > 0
			&& State.Energy > Thermal->DefaultEnergy)
		{
			const int32 CoolingDelta = FMath::Min<int32>(
				Thermal->CoolingPerStep,
				static_cast<int32>(State.Energy)
					- static_cast<int32>(Thermal->DefaultEnergy));
			ApplySpecificEnergyDelta(
				State,
				-CoolingDelta,
				OutBatch.ExplicitEnergyDelta);
		}
	}

	OutBatch.BaseStoreRevision = View.GetStoreRevision();
	for (const FMaterialElementAddress& Address : Touched)
	{
		const FMaterialElementState& Original = Before.FindChecked(Address);
		const FMaterialElementState& After = Working.FindChecked(Address);
		if (!(Original == After))
		{
			OutBatch.ElementDeltas.Add({ Address, Original, After });
		}
	}
	if (OutBatch.ElementDeltas.Num() > Context.MaxElementDeltas)
	{
		OutError = TEXT("local reaction element delta budget exceeded");
		return false;
	}
	OutBatch.TargetStoreRevision = OutBatch.ElementDeltas.IsEmpty()
		&& OutBatch.ParticleEmissions.IsEmpty()
		? OutBatch.BaseStoreRevision
		: OutBatch.BaseStoreRevision + 1;
	return OutBatch.IsValid(OutError);
}
