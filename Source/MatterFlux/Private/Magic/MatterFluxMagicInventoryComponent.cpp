#include "Magic/MatterFluxMagicInventoryComponent.h"

#include "GameFramework/PlayerState.h"
#include "IMatterFluxScriptRuntime.h"
#include "MatterFluxLog.h"
#include "Net/UnrealNetwork.h"
#include "Save/MatterFluxSaveTypes.h"

namespace MatterFluxMagicInventory
{
	constexpr int32 EquipmentSlotCount =
		MatterFlux::Magic::EquipmentSlotCount;

	FName ResolveRetiredSpellId(const FName SpellId)
	{
		return SpellId == TEXT("spell.ember_bolt")
			? FName(TEXT("spell.flame_jet"))
			: SpellId;
	}

	FMatterFluxOwnedWand* FindWand(
		TArray<FMatterFluxOwnedWand>& Wands,
		const FGuid WandId)
	{
		return Wands.FindByPredicate(
			[WandId](const FMatterFluxOwnedWand& Wand)
			{
				return Wand.InstanceId == WandId;
			});
	}

	FMatterFluxOwnedSpell* FindSpell(
		TArray<FMatterFluxOwnedSpell>& Spells,
		const FName SpellId)
	{
		return Spells.FindByPredicate(
			[SpellId](const FMatterFluxOwnedSpell& Spell)
			{
				return Spell.SpellId == SpellId;
			});
	}

	void AddSpell(
		TArray<FMatterFluxOwnedSpell>& Spells,
		const FName SpellId,
		const int32 Quantity)
	{
		if (Quantity == 0 || SpellId.IsNone())
		{
			return;
		}
		FMatterFluxOwnedSpell* Existing = FindSpell(Spells, SpellId);
		if (Existing)
		{
			Existing->Quantity += Quantity;
		}
		else if (Quantity > 0)
		{
			FMatterFluxOwnedSpell& Added = Spells.AddDefaulted_GetRef();
			Added.SpellId = SpellId;
			Added.Quantity = Quantity;
		}
		Spells.RemoveAll(
			[](const FMatterFluxOwnedSpell& Spell)
			{
				return Spell.Quantity <= 0;
			});
		Spells.Sort(
			[](const FMatterFluxOwnedSpell& A,
				const FMatterFluxOwnedSpell& B)
			{
				return A.SpellId.LexicalLess(B.SpellId);
			});
	}

	bool IsValidEquipmentSlot(const int32 Slot)
	{
		return Slot >= 0 && Slot < EquipmentSlotCount;
	}

	struct FProgressionEffectsPlan
	{
		TArray<FMatterFluxOwnedSpell> Spells;
		TArray<FMatterFluxOwnedWand> Wands;
		TArray<FGuid> EquippedWands;
		bool bChanged = false;
		bool bRewardsChanged = false;
	};

	bool BuildProgressionEffectsPlan(
		const FMatterFluxContentRegistry& Registry,
		const TArray<FMatterFluxOwnedSpell>& CurrentSpells,
		const TArray<FMatterFluxOwnedWand>& CurrentWands,
		const TArray<FGuid>& CurrentEquippedWands,
		const int32 ActiveEquipmentSlot,
		const int32 InventoryRevision,
		const uint64 PlayerSeed,
		const double Now,
		const TArray<FMatterFluxQuestRewardDefinition>& Rewards,
		const float WandManaRestoreAmount,
		FProgressionEffectsPlan& OutPlan,
		FString& OutError)
	{
		OutError.Reset();
		OutPlan = FProgressionEffectsPlan();
		if (CurrentEquippedWands.Num() != EquipmentSlotCount
			|| !IsValidEquipmentSlot(ActiveEquipmentSlot))
		{
			OutError = TEXT("equipment state must contain exactly five valid wand slots");
			return false;
		}
		if (!FMath::IsFinite(WandManaRestoreAmount)
			|| WandManaRestoreAmount < 0.0f)
		{
			OutError = TEXT("wand mana restoration amount must be finite and non-negative");
			return false;
		}

		OutPlan.Spells = CurrentSpells;
		OutPlan.Wands = CurrentWands;
		OutPlan.EquippedWands = CurrentEquippedWands;
		if (WandManaRestoreAmount > 0.0f)
		{
			FMatterFluxOwnedWand* Wand = FindWand(
				OutPlan.Wands,
				OutPlan.EquippedWands[ActiveEquipmentSlot]);
			const FMatterFluxWandDefinition* Definition = Wand
				? Registry.Wands.Find(Wand->DefinitionId) : nullptr;
			if (!Wand || !Definition || Wand->Mana >= Definition->ManaMax)
			{
				OutError = TEXT("active wand cannot receive mana");
				return false;
			}
			Wand->Mana = FMath::Min(
				Wand->Mana + WandManaRestoreAmount,
				Definition->ManaMax);
			Wand->LastManaUpdateServerTime = Now;
			OutPlan.bChanged = true;
		}

		for (int32 RewardIndex = 0; RewardIndex < Rewards.Num(); ++RewardIndex)
		{
			const FMatterFluxQuestRewardDefinition& Reward = Rewards[RewardIndex];
			if (Reward.Quantity <= 0)
			{
				OutError = TEXT("quest reward quantity must be positive");
				return false;
			}
			if (Reward.Kind == EMatterFluxQuestRewardKind::Item)
			{
				continue;
			}
			if (Reward.Kind == EMatterFluxQuestRewardKind::Spell)
			{
				if (!Registry.Spells.Contains(Reward.ContentId))
				{
					OutError = TEXT("quest reward references an unknown spell");
					return false;
				}
				AddSpell(OutPlan.Spells, Reward.ContentId, Reward.Quantity);
				OutPlan.bChanged = true;
				OutPlan.bRewardsChanged = true;
				continue;
			}
			if (Reward.Kind != EMatterFluxQuestRewardKind::Wand)
			{
				OutError = TEXT("quest reward kind is invalid");
				return false;
			}
			const FMatterFluxWandDefinition* Definition =
				Registry.Wands.Find(Reward.ContentId);
			if (!Definition)
			{
				OutError = TEXT("quest reward references an unknown wand");
				return false;
			}
			if (Reward.EquipmentSlot != INDEX_NONE
				&& !IsValidEquipmentSlot(Reward.EquipmentSlot))
			{
				OutError = TEXT("quest wand reward has an invalid equipment slot");
				return false;
			}
			for (int32 CopyIndex = 0; CopyIndex < Reward.Quantity; ++CopyIndex)
			{
				FMatterFluxOwnedWand& Wand = OutPlan.Wands.AddDefaulted_GetRef();
				Wand.InstanceId = FGuid::NewDeterministicGuid(
					FString::Printf(
						TEXT("MatterFlux.Reward.%s.Revision%d.Index%d.Copy%d.Owned%d"),
						*Reward.ContentId.ToString(), InventoryRevision,
						RewardIndex, CopyIndex, OutPlan.Wands.Num()),
					PlayerSeed);
				Wand.DefinitionId = Reward.ContentId;
				Wand.SpellSlots.Init(NAME_None, Definition->Capacity);
				for (int32 DeckIndex = 0;
					DeckIndex < Definition->StarterDeck.Num(); ++DeckIndex)
				{
					Wand.SpellSlots[DeckIndex] =
						Definition->StarterDeck[DeckIndex];
				}
				Wand.Mana = Definition->ManaMax;
				Wand.LastManaUpdateServerTime = Now;
				if (CopyIndex == 0 && Reward.EquipmentSlot != INDEX_NONE)
				{
					OutPlan.EquippedWands[Reward.EquipmentSlot] = Wand.InstanceId;
				}
			}
			OutPlan.bChanged = true;
			OutPlan.bRewardsChanged = true;
		}
		return true;
	}
}

void FMatterFluxOwnedSpell::PostReplicatedAdd(
	const FMatterFluxOwnedSpellList& List)
{
	if (List.Owner.IsValid())
	{
		List.Owner->HandleReplicatedInventoryChanged();
	}
}

void FMatterFluxOwnedSpell::PostReplicatedChange(
	const FMatterFluxOwnedSpellList& List)
{
	if (List.Owner.IsValid())
	{
		List.Owner->HandleReplicatedInventoryChanged();
	}
}

void FMatterFluxOwnedSpell::PreReplicatedRemove(
	const FMatterFluxOwnedSpellList& List)
{
	if (List.Owner.IsValid())
	{
		List.Owner->HandleReplicatedInventoryChanged();
	}
}

void FMatterFluxOwnedWand::PostReplicatedAdd(
	const FMatterFluxOwnedWandList& List)
{
	if (List.Owner.IsValid())
	{
		List.Owner->HandleReplicatedInventoryChanged();
	}
}

void FMatterFluxOwnedWand::PostReplicatedChange(
	const FMatterFluxOwnedWandList& List)
{
	if (List.Owner.IsValid())
	{
		List.Owner->HandleReplicatedInventoryChanged();
	}
}

void FMatterFluxOwnedWand::PreReplicatedRemove(
	const FMatterFluxOwnedWandList& List)
{
	if (List.Owner.IsValid())
	{
		List.Owner->HandleReplicatedInventoryChanged();
	}
}

bool FMatterFluxMagicInventoryRules::ApplyEdit(
	const FMatterFluxContentRegistry& Registry,
	TArray<FMatterFluxOwnedSpell>& Spells,
	TArray<FMatterFluxOwnedWand>& Wands,
	TArray<FGuid>& EquippedWands,
	int32& ActiveEquipmentSlot,
	const FMatterFluxMagicEdit& Edit,
	FString& OutError)
{
	using namespace MatterFluxMagicInventory;
	OutError.Reset();
	if (EquippedWands.Num() != EquipmentSlotCount)
	{
		OutError = TEXT("equipment state must contain exactly five wand slots");
		return false;
	}

	switch (Edit.Type)
	{
	case EMatterFluxMagicEditType::EquipWand:
	{
		if (!IsValidEquipmentSlot(Edit.EquipmentSlot))
		{
			OutError = TEXT("equipment slot is out of range");
			return false;
		}
		FMatterFluxOwnedWand* Wand = FindWand(Wands, Edit.WandId);
		if (!Wand || !Registry.Wands.Contains(Wand->DefinitionId))
		{
			OutError = TEXT("wand is not owned or its definition is missing");
			return false;
		}
		if (EquippedWands[Edit.EquipmentSlot] == Edit.WandId)
		{
			OutError = TEXT("wand is already equipped in that slot");
			return false;
		}
		for (FGuid& Equipped : EquippedWands)
		{
			if (Equipped == Edit.WandId)
			{
				Equipped.Invalidate();
			}
		}
		EquippedWands[Edit.EquipmentSlot] = Edit.WandId;
		return true;
	}

	case EMatterFluxMagicEditType::UnequipWand:
		if (!IsValidEquipmentSlot(Edit.EquipmentSlot)
			|| !EquippedWands[Edit.EquipmentSlot].IsValid())
		{
			OutError = TEXT("equipment slot is empty or out of range");
			return false;
		}
		EquippedWands[Edit.EquipmentSlot].Invalidate();
		return true;

	case EMatterFluxMagicEditType::SelectEquipmentSlot:
		if (!IsValidEquipmentSlot(Edit.EquipmentSlot)
			|| ActiveEquipmentSlot == Edit.EquipmentSlot)
		{
			OutError = TEXT("active equipment slot is unchanged or out of range");
			return false;
		}
		ActiveEquipmentSlot = Edit.EquipmentSlot;
		return true;

	case EMatterFluxMagicEditType::AssignSpell:
	{
		FMatterFluxOwnedWand* Wand = FindWand(Wands, Edit.WandId);
		const FMatterFluxWandDefinition* WandDefinition = Wand
			? Registry.Wands.Find(Wand->DefinitionId)
			: nullptr;
		FMatterFluxOwnedSpell* OwnedSpell =
			FindSpell(Spells, Edit.SpellId);
		if (!Wand
			|| !WandDefinition
			|| !Registry.Spells.Contains(Edit.SpellId)
			|| !OwnedSpell
			|| OwnedSpell->Quantity <= 0
			|| Edit.ToSpellSlot < 0
			|| Edit.ToSpellSlot >= WandDefinition->Capacity
			|| Edit.ToSpellSlot >= Wand->SpellSlots.Num())
		{
			OutError = TEXT("spell assignment references an invalid wand, spell, or slot");
			return false;
		}
		const FName Previous = Wand->SpellSlots[Edit.ToSpellSlot];
		if (Previous == Edit.SpellId)
		{
			OutError = TEXT("spell is already assigned to that slot");
			return false;
		}
		AddSpell(Spells, Previous, 1);
		AddSpell(Spells, Edit.SpellId, -1);
		Wand->SpellSlots[Edit.ToSpellSlot] = Edit.SpellId;
		Wand->DeckCursor = 0;
		return true;
	}

	case EMatterFluxMagicEditType::RemoveSpell:
	{
		FMatterFluxOwnedWand* Wand = FindWand(Wands, Edit.WandId);
		if (!Wand
			|| Edit.FromSpellSlot < 0
			|| Edit.FromSpellSlot >= Wand->SpellSlots.Num()
			|| Wand->SpellSlots[Edit.FromSpellSlot].IsNone())
		{
			OutError = TEXT("spell removal references an empty or invalid slot");
			return false;
		}
		AddSpell(Spells, Wand->SpellSlots[Edit.FromSpellSlot], 1);
		Wand->SpellSlots[Edit.FromSpellSlot] = NAME_None;
		Wand->DeckCursor = 0;
		return true;
	}

	case EMatterFluxMagicEditType::SwapSpellSlots:
	{
		FMatterFluxOwnedWand* Wand = FindWand(Wands, Edit.WandId);
		if (!Wand
			|| Edit.FromSpellSlot < 0
			|| Edit.ToSpellSlot < 0
			|| Edit.FromSpellSlot >= Wand->SpellSlots.Num()
			|| Edit.ToSpellSlot >= Wand->SpellSlots.Num()
			|| Edit.FromSpellSlot == Edit.ToSpellSlot)
		{
			OutError = TEXT("spell swap references invalid slots");
			return false;
		}
		Wand->SpellSlots.Swap(Edit.FromSpellSlot, Edit.ToSpellSlot);
		Wand->DeckCursor = 0;
		return true;
	}
	default:
		OutError = TEXT("unknown magic inventory edit type");
		return false;
	}
}

UMatterFluxMagicInventoryComponent::UMatterFluxMagicInventoryComponent()
{
	SetIsReplicatedByDefault(true);
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.2f;
}

void UMatterFluxMagicInventoryComponent::BeginPlay()
{
	Super::BeginPlay();
	OwnedSpells.Owner = this;
	OwnedWands.Owner = this;
	ContentReloadedHandle =
		IMatterFluxScriptRuntime::Get().OnContentReloaded().AddUObject(
			this,
			&UMatterFluxMagicInventoryComponent::HandleContentReloaded);
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		InitializeStarterLoadout();
	}
}

void UMatterFluxMagicInventoryComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	if (ContentReloadedHandle.IsValid()
		&& IMatterFluxScriptRuntime::IsAvailable())
	{
		IMatterFluxScriptRuntime::Get().OnContentReloaded().Remove(
			ContentReloadedHandle);
	}
	ContentReloadedHandle.Reset();
	Super::EndPlay(EndPlayReason);
}

void UMatterFluxMagicInventoryComponent::TickComponent(
	const float DeltaTime,
	const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!GetOwner() || !GetOwner()->HasAuthority() || !GetWorld())
	{
		return;
	}
	// The Lua registry may become active after PlayerState::BeginPlay during
	// editor startup or travel. Keep starter initialization idempotent and retry
	// until the authoritative registry is ready.
	if (InventoryRevision == 0
		&& OwnedWands.Items.IsEmpty()
		&& OwnedSpells.Items.IsEmpty())
	{
		InitializeStarterLoadout();
	}
	const double Now = GetWorld()->GetTimeSeconds();
	bool bChanged = false;
	for (FMatterFluxOwnedWand& Wand : OwnedWands.Items)
	{
		if (RegenerateWandMana(Wand, Now))
		{
			OwnedWands.MarkItemDirty(Wand);
			bChanged = true;
		}
	}
	if (bChanged)
	{
		InventoryChanged.Broadcast();
		GetOwner()->ForceNetUpdate();
	}
}

void UMatterFluxMagicInventoryComponent::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(
		UMatterFluxMagicInventoryComponent,
		OwnedSpells,
		COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(
		UMatterFluxMagicInventoryComponent,
		OwnedWands,
		COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(
		UMatterFluxMagicInventoryComponent,
		EquippedWands,
		COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(
		UMatterFluxMagicInventoryComponent,
		ActiveEquipmentSlot,
		COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(
		UMatterFluxMagicInventoryComponent,
		InventoryRevision,
		COND_OwnerOnly);
}

const FMatterFluxOwnedWand* UMatterFluxMagicInventoryComponent::FindWand(
	const FGuid WandId) const
{
	return OwnedWands.Items.FindByPredicate(
		[WandId](const FMatterFluxOwnedWand& Wand)
		{
			return Wand.InstanceId == WandId;
		});
}

FGuid UMatterFluxMagicInventoryComponent::GetActiveWandId() const
{
	return GetEquippedWandId(ActiveEquipmentSlot);
}

FGuid UMatterFluxMagicInventoryComponent::GetEquippedWandId(
	const int32 EquipmentSlot) const
{
	return EquippedWands.IsValidIndex(EquipmentSlot)
		? EquippedWands[EquipmentSlot]
		: FGuid();
}

void UMatterFluxMagicInventoryComponent::RequestEdit(
	const FMatterFluxMagicEdit& Edit)
{
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		FString Error;
		if (!ApplyEditAuthority(Edit, Error))
		{
			UE_LOG(LogMatterFlux, Warning,
				TEXT("Rejected local magic edit: %s"), *Error);
		}
	}
	else
	{
		ServerApplyEdit(Edit);
	}
}

bool UMatterFluxMagicInventoryComponent::ApplyEditAuthority(
	const FMatterFluxMagicEdit& Edit,
	FString& OutError)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		OutError = TEXT("magic inventory edits require authority");
		return false;
	}
	if (Edit.ExpectedRevision != InventoryRevision)
	{
		OutError = FString::Printf(
			TEXT("stale magic inventory revision %d; expected %d"),
			Edit.ExpectedRevision,
			InventoryRevision);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!Registry.IsValid())
	{
		OutError = TEXT("magic content registry is unavailable");
		return false;
	}

	TArray<FMatterFluxOwnedSpell> NextSpells = OwnedSpells.Items;
	TArray<FMatterFluxOwnedWand> NextWands = OwnedWands.Items;
	TArray<FGuid> NextEquipped = EquippedWands;
	int32 NextActiveSlot = ActiveEquipmentSlot;
	if (!FMatterFluxMagicInventoryRules::ApplyEdit(
		*Registry,
		NextSpells,
		NextWands,
		NextEquipped,
		NextActiveSlot,
		Edit,
		OutError))
	{
		return false;
	}

	OwnedSpells.Items = MoveTemp(NextSpells);
	OwnedWands.Items = MoveTemp(NextWands);
	EquippedWands = MoveTemp(NextEquipped);
	ActiveEquipmentSlot = NextActiveSlot;
	++InventoryRevision;
	MarkAllInventoryDirty();
	InventoryChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
	return true;
}

bool UMatterFluxMagicInventoryComponent::CommitActiveCastAuthority(
	const int32 EventSeed,
	FMatterFluxWandCastPlan& OutPlan,
	FGuid& OutWandId,
	FString& OutError)
{
	return ExecuteActiveCastAuthority(
		EventSeed,
		[](const FMatterFluxWandCastPlan&) { return true; },
		OutPlan,
		OutWandId,
		OutError);
}

bool UMatterFluxMagicInventoryComponent::ExecuteActiveCastAuthority(
	const int32 EventSeed,
	TFunctionRef<bool(const FMatterFluxWandCastPlan&)> Executor,
	FMatterFluxWandCastPlan& OutPlan,
	FGuid& OutWandId,
	FString& OutError)
{
	return ExecuteCastAuthority(
		ActiveEquipmentSlot,
		EventSeed,
		Executor,
		OutPlan,
		OutWandId,
		OutError);
}

bool UMatterFluxMagicInventoryComponent::ExecuteCastAuthority(
	const int32 EquipmentSlot,
	const int32 EventSeed,
	TFunctionRef<bool(const FMatterFluxWandCastPlan&)> Executor,
	FMatterFluxWandCastPlan& OutPlan,
	FGuid& OutWandId,
	FString& OutError)
{
	OutPlan = FMatterFluxWandCastPlan();
	OutWandId.Invalidate();
	if (!GetOwner() || !GetOwner()->HasAuthority() || !GetWorld())
	{
		OutError = TEXT("wand casts require authority and a world");
		return false;
	}
	FMatterFluxOwnedWand* Wand = MatterFluxMagicInventory::FindWand(
		OwnedWands.Items,
		GetEquippedWandId(EquipmentSlot));
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	const FMatterFluxWandDefinition* Definition =
		Wand && Registry.IsValid()
			? Registry->Wands.Find(Wand->DefinitionId)
			: nullptr;
	if (!Wand || !Definition)
	{
		OutError = FString::Printf(
			TEXT("no valid wand is equipped in slot %d"),
			EquipmentSlot);
		return false;
	}
	const double Now = GetWorld()->GetTimeSeconds();
	RegenerateWandMana(*Wand, Now);
	if (Now + UE_DOUBLE_SMALL_NUMBER < Wand->NextCastServerTime)
	{
		OutError = TEXT("wand is still in cast delay or recharge");
		return false;
	}

	FMatterFluxWandProgramState State;
	State.Mana = Wand->Mana;
	State.DeckCursor = Wand->DeckCursor;
	State.CastSerial = Wand->CastSerial;
	FMatterFluxWandCastPlan Candidate;
	if (!FMatterFluxWandProgram::Evaluate(
		*Registry,
		Wand->DefinitionId,
		Wand->SpellSlots,
		State,
		EventSeed,
		Candidate,
		OutError))
	{
		return false;
	}
	if (!Executor(Candidate))
	{
		OutError = TEXT("wand cast executor rejected the compiled plan");
		return false;
	}

	Wand->Mana = Candidate.NextState.Mana;
	Wand->DeckCursor = Candidate.NextState.DeckCursor;
	Wand->CastSerial = Candidate.NextState.CastSerial;
	Wand->LastManaUpdateServerTime = Now;
	Wand->NextCastServerTime = Now
		+ Candidate.CastDelay
		+ (Candidate.bDeckExhausted ? Candidate.RechargeTime : 0.0f);
	OwnedWands.MarkItemDirty(*Wand);
	InventoryChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
	OutWandId = Wand->InstanceId;
	OutPlan = MoveTemp(Candidate);
	return true;
}

bool UMatterFluxMagicInventoryComponent::CaptureSaveState(
	FMatterFluxMagicInventorySaveState& OutState,
	FString& OutError) const
{
	OutState = FMatterFluxMagicInventorySaveState();
	OutError.Reset();
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		OutError = TEXT("magic inventory save capture requires authority");
		return false;
	}
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	OutState.Spells.Reserve(OwnedSpells.Items.Num());
	for (const FMatterFluxOwnedSpell& Spell : OwnedSpells.Items)
	{
		FMatterFluxSavedSpell& Saved = OutState.Spells.AddDefaulted_GetRef();
		Saved.SpellId = Spell.SpellId;
		Saved.Quantity = Spell.Quantity;
	}
	OutState.Wands.Reserve(OwnedWands.Items.Num());
	for (const FMatterFluxOwnedWand& Wand : OwnedWands.Items)
	{
		FMatterFluxSavedWand& Saved = OutState.Wands.AddDefaulted_GetRef();
		Saved.InstanceId = Wand.InstanceId;
		Saved.DefinitionId = Wand.DefinitionId;
		Saved.SpellSlots = Wand.SpellSlots;
		Saved.Mana = Wand.Mana;
		Saved.DeckCursor = Wand.DeckCursor;
		Saved.CastSerial = Wand.CastSerial;
		Saved.CastCooldownRemaining = static_cast<float>(FMath::Clamp(
			Wand.NextCastServerTime - Now,
			0.0,
			3600.0));
	}
	OutState.EquippedWands = EquippedWands;
	OutState.ActiveEquipmentSlot = ActiveEquipmentSlot;
	OutState.InventoryRevision = FMath::Max(InventoryRevision, 1);
	return true;
}

bool UMatterFluxMagicInventoryComponent::RestoreSaveStateAuthority(
	const FMatterFluxMagicInventorySaveState& State,
	FString& OutError)
{
	using namespace MatterFluxMagicInventory;
	OutError.Reset();
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		OutError = TEXT("magic inventory save restore requires authority");
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!Registry.IsValid()
		|| State.EquippedWands.Num() != EquipmentSlotCount
		|| !IsValidEquipmentSlot(State.ActiveEquipmentSlot)
		|| State.InventoryRevision < 0
		|| State.Spells.Num() > 256
		|| State.Wands.Num() > 256)
	{
		OutError = TEXT("saved magic inventory metadata is invalid");
		return false;
	}

	TArray<FMatterFluxOwnedSpell> CandidateSpells;
	TSet<FName> SeenSpells;
	for (const FMatterFluxSavedSpell& Saved : State.Spells)
	{
		const FName RuntimeSpellId = ResolveRetiredSpellId(Saved.SpellId);
		if (Saved.SpellId.IsNone()
			|| Saved.Quantity <= 0
			|| !Registry->Spells.Contains(RuntimeSpellId)
			|| SeenSpells.Contains(Saved.SpellId))
		{
			OutError = TEXT("saved magic inventory contains an invalid spell stack");
			return false;
		}
		SeenSpells.Add(Saved.SpellId);
		FMatterFluxOwnedSpell* Existing = FindSpell(
			CandidateSpells,
			RuntimeSpellId);
		if (Existing)
		{
			const int64 MergedQuantity = static_cast<int64>(Existing->Quantity)
				+ static_cast<int64>(Saved.Quantity);
			if (MergedQuantity > MAX_int32)
			{
				OutError = TEXT("saved spell migration exceeds the quantity limit");
				return false;
			}
			Existing->Quantity = static_cast<int32>(MergedQuantity);
		}
		else
		{
			FMatterFluxOwnedSpell& Spell = CandidateSpells.AddDefaulted_GetRef();
			Spell.SpellId = RuntimeSpellId;
			Spell.Quantity = Saved.Quantity;
		}
	}

	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	TArray<FMatterFluxOwnedWand> CandidateWands;
	TSet<FGuid> SeenWands;
	for (const FMatterFluxSavedWand& Saved : State.Wands)
	{
		const FMatterFluxWandDefinition* Definition =
			Registry->Wands.Find(Saved.DefinitionId);
		if (!Saved.InstanceId.IsValid()
			|| SeenWands.Contains(Saved.InstanceId)
			|| !Definition
			|| Saved.SpellSlots.Num() != Definition->Capacity
			|| !FMath::IsFinite(Saved.Mana)
			|| Saved.Mana < 0.0f
			|| Saved.DeckCursor < 0
			|| Saved.CastSerial < 0
			|| !FMath::IsFinite(Saved.CastCooldownRemaining)
			|| Saved.CastCooldownRemaining < 0.0f
			|| Saved.CastCooldownRemaining > 3600.0f)
		{
			OutError = TEXT("saved magic inventory contains an invalid wand");
			return false;
		}
		for (const FName SavedSpellId : Saved.SpellSlots)
		{
			const FName RuntimeSpellId = ResolveRetiredSpellId(SavedSpellId);
			if (!RuntimeSpellId.IsNone()
				&& !Registry->Spells.Contains(RuntimeSpellId))
			{
				OutError = TEXT("saved wand references an unknown spell");
				return false;
			}
		}
		SeenWands.Add(Saved.InstanceId);
		FMatterFluxOwnedWand& Wand = CandidateWands.AddDefaulted_GetRef();
		Wand.InstanceId = Saved.InstanceId;
		Wand.DefinitionId = Saved.DefinitionId;
		Wand.SpellSlots = Saved.SpellSlots;
		for (FName& SpellId : Wand.SpellSlots)
		{
			SpellId = ResolveRetiredSpellId(SpellId);
		}
		Wand.Mana = FMath::Clamp(Saved.Mana, 0.0f, Definition->ManaMax);
		Wand.DeckCursor = Saved.DeckCursor;
		Wand.CastSerial = Saved.CastSerial;
		Wand.NextCastServerTime = Now + Saved.CastCooldownRemaining;
		Wand.LastManaUpdateServerTime = Now;
	}

	TSet<FGuid> SeenEquipped;
	for (const FGuid Equipped : State.EquippedWands)
	{
		if (Equipped.IsValid()
			&& (!SeenWands.Contains(Equipped)
				|| SeenEquipped.Contains(Equipped)))
		{
			OutError = TEXT("saved equipment references an invalid or duplicate wand");
			return false;
		}
		if (Equipped.IsValid())
		{
			SeenEquipped.Add(Equipped);
		}
	}

	OwnedSpells.Items = MoveTemp(CandidateSpells);
	OwnedWands.Items = MoveTemp(CandidateWands);
	EquippedWands = State.EquippedWands;
	ActiveEquipmentSlot = State.ActiveEquipmentSlot;
	InventoryRevision = FMath::Max(State.InventoryRevision, 1);
	OwnedSpells.Owner = this;
	OwnedWands.Owner = this;
	MarkAllInventoryDirty();
	InventoryChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
	return true;
}

bool UMatterFluxMagicInventoryComponent::ResetToStarterLoadoutAuthority(
	FString& OutError)
{
	OutError.Reset();
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		OutError = TEXT("starter loadout reset requires authority");
		return false;
	}
	OwnedSpells.Items.Reset();
	OwnedWands.Items.Reset();
	EquippedWands.Reset();
	ActiveEquipmentSlot = 0;
	InventoryRevision = 0;
	InitializeStarterLoadout();
	if (OwnedWands.Items.IsEmpty())
	{
		OutError = TEXT("starter loadout could not be initialized");
		return false;
	}
	return true;
}

bool UMatterFluxMagicInventoryComponent::ResetToEmptyLoadoutAuthority(
	FString& OutError)
{
	OutError.Reset();
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		OutError = TEXT("empty loadout reset requires authority");
		return false;
	}
	OwnedSpells.Items.Reset();
	OwnedWands.Items.Reset();
	EquippedWands.Init(FGuid(), MatterFluxMagicInventory::EquipmentSlotCount);
	ActiveEquipmentSlot = 0;
	InventoryRevision = FMath::Max(InventoryRevision + 1, 1);
	OwnedSpells.Owner = this;
	OwnedWands.Owner = this;
	MarkAllInventoryDirty();
	InventoryChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
	return true;
}

bool UMatterFluxMagicInventoryComponent::ApplyQuestRewardsAuthority(
	const TArray<FMatterFluxQuestRewardDefinition>& Rewards,
	FString& OutError)
{
	return ApplyProgressionEffectsAuthority(Rewards, 0.0f, OutError);
}

bool UMatterFluxMagicInventoryComponent::RestoreEquippedWandManaAuthority(
	const float Amount,
	FString& OutError)
{
	const TArray<FMatterFluxQuestRewardDefinition> NoRewards;
	return ApplyProgressionEffectsAuthority(NoRewards, Amount, OutError);
}

bool UMatterFluxMagicInventoryComponent::ApplyProgressionEffectsAuthority(
	const TArray<FMatterFluxQuestRewardDefinition>& Rewards,
	const float WandManaRestoreAmount,
	FString& OutError)
{
	using namespace MatterFluxMagicInventory;
	OutError.Reset();
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		OutError = TEXT("progression magic effects require authority");
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!Registry.IsValid())
	{
		OutError = TEXT("magic content registry is unavailable");
		return false;
	}
	const APlayerState* PlayerState = Cast<APlayerState>(GetOwner());
	const uint64 PlayerSeed = static_cast<uint64>(static_cast<uint32>(
		PlayerState ? FMath::Max(0, PlayerState->GetPlayerId()) : 0));
	FProgressionEffectsPlan Plan;
	if (!BuildProgressionEffectsPlan(
		*Registry,
		OwnedSpells.Items,
		OwnedWands.Items,
		EquippedWands,
		ActiveEquipmentSlot,
		InventoryRevision,
		PlayerSeed,
		GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0,
		Rewards,
		WandManaRestoreAmount,
		Plan,
		OutError))
	{
		return false;
	}
	if (!Plan.bChanged)
	{
		return true;
	}

	OwnedSpells.Items = MoveTemp(Plan.Spells);
	OwnedWands.Items = MoveTemp(Plan.Wands);
	EquippedWands = MoveTemp(Plan.EquippedWands);
	if (Plan.bRewardsChanged)
	{
		++InventoryRevision;
	}
	MarkAllInventoryDirty();
	InventoryChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
	return true;
}

void UMatterFluxMagicInventoryComponent::HandleReplicatedInventoryChanged()
{
	OwnedSpells.Owner = this;
	OwnedWands.Owner = this;
	InventoryChanged.Broadcast();
}

void UMatterFluxMagicInventoryComponent::HandleContentReloaded(
	const FMatterFluxContentRegistryPtr Registry)
{
	if (!Registry.IsValid() || !GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	bool bChanged = false;
	int32 RetiredEmberQuantity = 0;
	for (const FMatterFluxOwnedSpell& Spell : OwnedSpells.Items)
	{
		if (Spell.SpellId == TEXT("spell.ember_bolt"))
		{
			RetiredEmberQuantity += Spell.Quantity;
		}
	}
	if (RetiredEmberQuantity > 0
		&& Registry->Spells.Contains(TEXT("spell.flame_jet")))
	{
		OwnedSpells.Items.RemoveAll([](const FMatterFluxOwnedSpell& Spell)
		{
			return Spell.SpellId == TEXT("spell.ember_bolt");
		});
		MatterFluxMagicInventory::AddSpell(
			OwnedSpells.Items,
			TEXT("spell.flame_jet"),
			RetiredEmberQuantity);
		bChanged = true;
	}
	for (FMatterFluxOwnedWand& Wand : OwnedWands.Items)
	{
		bool bWandChanged = false;
		const FMatterFluxWandDefinition* Definition =
			Registry->Wands.Find(Wand.DefinitionId);
		if (!Definition)
		{
			continue;
		}
		while (Wand.SpellSlots.Num() > Definition->Capacity)
		{
			const FName Removed =
				MatterFluxMagicInventory::ResolveRetiredSpellId(
					Wand.SpellSlots.Pop());
			if (Registry->Spells.Contains(Removed))
			{
				MatterFluxMagicInventory::AddSpell(
					OwnedSpells.Items,
					Removed,
					1);
			}
			bWandChanged = true;
		}
		if (Wand.SpellSlots.Num() < Definition->Capacity)
		{
			Wand.SpellSlots.SetNum(Definition->Capacity);
			bWandChanged = true;
		}
		for (FName& SpellId : Wand.SpellSlots)
		{
			const FName RuntimeSpellId =
				MatterFluxMagicInventory::ResolveRetiredSpellId(SpellId);
			if (RuntimeSpellId != SpellId)
			{
				SpellId = RuntimeSpellId;
				bWandChanged = true;
			}
			if (!SpellId.IsNone() && !Registry->Spells.Contains(SpellId))
			{
				SpellId = NAME_None;
				bWandChanged = true;
			}
		}
		const float ClampedMana = FMath::Clamp(
			Wand.Mana,
			0.0f,
			Definition->ManaMax);
		if (!FMath::IsNearlyEqual(ClampedMana, Wand.Mana))
		{
			Wand.Mana = ClampedMana;
			bWandChanged = true;
		}
		if (bWandChanged)
		{
			Wand.DeckCursor = 0;
			bChanged = true;
		}
	}

	if (bChanged)
	{
		++InventoryRevision;
		MarkAllInventoryDirty();
		InventoryChanged.Broadcast();
		GetOwner()->ForceNetUpdate();
	}
	else if (OwnedWands.Items.IsEmpty() && OwnedSpells.Items.IsEmpty())
	{
		InitializeStarterLoadout();
	}
}

void UMatterFluxMagicInventoryComponent::InitializeStarterLoadout()
{
	using namespace MatterFluxMagicInventory;
	if (!OwnedWands.Items.IsEmpty() || !OwnedSpells.Items.IsEmpty())
	{
		return;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!Registry.IsValid())
	{
		return;
	}
	TArray<FName> WandIds;
	Registry->Wands.GetKeys(WandIds);
	WandIds.Sort(FNameLexicalLess());
	WandIds = WandIds.FilterByPredicate(
		[&Registry](const FName Id)
		{
			const FMatterFluxWandDefinition& Wand =
				Registry->Wands.FindChecked(Id);
			return Wand.StarterEquipmentSlot >= 0 || Wand.StarterCount > 0;
		});
	if (WandIds.IsEmpty())
	{
		return;
	}

	TArray<FName> SpellIds;
	Registry->Spells.GetKeys(SpellIds);
	SpellIds.Sort(FNameLexicalLess());
	for (const FName SpellId : SpellIds)
	{
		const int32 Count =
			Registry->Spells.FindChecked(SpellId).StarterCount;
		if (Count > 0)
		{
			AddSpell(OwnedSpells.Items, SpellId, Count);
		}
	}

	const APlayerState* PlayerState = Cast<APlayerState>(GetOwner());
	const uint64 PlayerSeed = static_cast<uint64>(static_cast<uint32>(
		PlayerState ? FMath::Max(0, PlayerState->GetPlayerId()) : 0));
	EquippedWands.Init(FGuid(), EquipmentSlotCount);
	for (const FName WandId : WandIds)
	{
		const FMatterFluxWandDefinition& StarterWand =
			Registry->Wands.FindChecked(WandId);
		const int32 Copies = FMath::Max(
			StarterWand.StarterCount,
			StarterWand.StarterEquipmentSlot >= 0 ? 1 : 0);
		for (int32 CopyIndex = 0; CopyIndex < Copies; ++CopyIndex)
		{
			FMatterFluxOwnedWand& Wand =
				OwnedWands.Items.AddDefaulted_GetRef();
			const FString GuidKey = StarterWand.StarterEquipmentSlot >= 0
				&& CopyIndex == 0
				? FString::Printf(
					TEXT("MatterFlux.Starter.%s.Slot%d"),
					*StarterWand.Id.ToString(),
					StarterWand.StarterEquipmentSlot)
				: FString::Printf(
					TEXT("MatterFlux.Starter.%s.Owned.Copy%d"),
					*StarterWand.Id.ToString(), CopyIndex);
			Wand.InstanceId =
				FGuid::NewDeterministicGuid(GuidKey, PlayerSeed);
			Wand.DefinitionId = StarterWand.Id;
			Wand.SpellSlots.Init(NAME_None, StarterWand.Capacity);
			for (int32 DeckIndex = 0;
				DeckIndex < StarterWand.StarterDeck.Num();
				++DeckIndex)
			{
				Wand.SpellSlots[DeckIndex] =
					StarterWand.StarterDeck[DeckIndex];
			}
			Wand.Mana = StarterWand.ManaMax;
			Wand.LastManaUpdateServerTime = GetWorld()
				? GetWorld()->GetTimeSeconds()
				: 0.0;
			if (CopyIndex == 0 && StarterWand.StarterEquipmentSlot >= 0)
			{
				EquippedWands[StarterWand.StarterEquipmentSlot] =
					Wand.InstanceId;
			}
		}
	}
	ActiveEquipmentSlot = 0;
	InventoryRevision = 1;
	MarkAllInventoryDirty();
	InventoryChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
}

bool UMatterFluxMagicInventoryComponent::RegenerateWandMana(
	FMatterFluxOwnedWand& Wand,
	const double Now)
{
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	const FMatterFluxWandDefinition* Definition = Registry.IsValid()
		? Registry->Wands.Find(Wand.DefinitionId)
		: nullptr;
	if (!Definition || !FMath::IsFinite(Now))
	{
		return false;
	}
	if (Wand.LastManaUpdateServerTime <= 0.0)
	{
		Wand.LastManaUpdateServerTime = Now;
		return false;
	}
	const float Previous = Wand.Mana;
	const double Elapsed = FMath::Max(
		0.0,
		Now - Wand.LastManaUpdateServerTime);
	Wand.Mana = FMath::Clamp(
		Wand.Mana
			+ Definition->ManaRechargePerSecond
				* static_cast<float>(Elapsed),
		0.0f,
		Definition->ManaMax);
	Wand.LastManaUpdateServerTime = Now;
	return !FMath::IsNearlyEqual(Previous, Wand.Mana, 0.01f);
}

void UMatterFluxMagicInventoryComponent::MarkAllInventoryDirty()
{
	OwnedSpells.MarkArrayDirty();
	for (FMatterFluxOwnedSpell& Spell : OwnedSpells.Items)
	{
		OwnedSpells.MarkItemDirty(Spell);
	}
	OwnedWands.MarkArrayDirty();
	for (FMatterFluxOwnedWand& Wand : OwnedWands.Items)
	{
		OwnedWands.MarkItemDirty(Wand);
	}
}

bool UMatterFluxMagicInventoryComponent::ServerApplyEdit_Validate(
	const FMatterFluxMagicEdit Edit)
{
	return Edit.ExpectedRevision >= 0
		&& Edit.ExpectedRevision < MAX_int32
		&& static_cast<uint8>(Edit.Type)
			<= static_cast<uint8>(
				EMatterFluxMagicEditType::SwapSpellSlots)
		&& Edit.EquipmentSlot >= INDEX_NONE
		&& Edit.EquipmentSlot < MatterFluxMagicInventory::EquipmentSlotCount
		&& Edit.FromSpellSlot >= INDEX_NONE
		&& Edit.FromSpellSlot < 32
		&& Edit.ToSpellSlot >= INDEX_NONE
		&& Edit.ToSpellSlot < 32
		&& Edit.SpellId.ToString().Len() <= 64;
}

void UMatterFluxMagicInventoryComponent::ServerApplyEdit_Implementation(
	const FMatterFluxMagicEdit Edit)
{
	FString Error;
	if (!ApplyEditAuthority(Edit, Error))
	{
		UE_LOG(LogMatterFlux, Warning,
			TEXT("Rejected client magic edit for %s: %s"),
			*GetNameSafe(GetOwner()),
			*Error);
	}
}

void UMatterFluxMagicInventoryComponent::OnRep_EquipmentState()
{
	HandleReplicatedInventoryChanged();
}
