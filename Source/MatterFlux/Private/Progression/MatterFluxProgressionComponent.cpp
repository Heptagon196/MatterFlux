#include "Progression/MatterFluxProgressionComponent.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "Game/MatterFluxPlayerState.h"
#include "GAS/MatterFluxPlayerAttributeSet.h"
#include "GameplayTagContainer.h"
#include "IMatterFluxScriptRuntime.h"
#include "Magic/MatterFluxMagicInventoryComponent.h"
#include "MatterFluxLog.h"
#include "Net/UnrealNetwork.h"
#include "Save/MatterFluxSaveTypes.h"
#include "TimerManager.h"

void FMatterFluxItemStack::PostReplicatedAdd(
	const FMatterFluxItemStackList& List)
{
	if (List.Owner.IsValid()) List.Owner->HandleReplicatedProgressionChanged();
}

void FMatterFluxItemStack::PostReplicatedChange(
	const FMatterFluxItemStackList& List)
{
	if (List.Owner.IsValid()) List.Owner->HandleReplicatedProgressionChanged();
}

void FMatterFluxItemStack::PreReplicatedRemove(
	const FMatterFluxItemStackList& List)
{
	if (List.Owner.IsValid()) List.Owner->HandleReplicatedProgressionChanged();
}

void FMatterFluxQuestState::PostReplicatedAdd(
	const FMatterFluxQuestStateList& List)
{
	if (List.Owner.IsValid()) List.Owner->HandleReplicatedProgressionChanged();
}

void FMatterFluxQuestState::PostReplicatedChange(
	const FMatterFluxQuestStateList& List)
{
	if (List.Owner.IsValid()) List.Owner->HandleReplicatedProgressionChanged();
}

void FMatterFluxQuestState::PreReplicatedRemove(
	const FMatterFluxQuestStateList& List)
{
	if (List.Owner.IsValid()) List.Owner->HandleReplicatedProgressionChanged();
}

namespace MatterFluxProgressionRules
{
	FMatterFluxItemStack* FindItem(
		TArray<FMatterFluxItemStack>& Items,
		const FName ItemId)
	{
		return Items.FindByPredicate([ItemId](const FMatterFluxItemStack& Item)
		{
			return Item.ItemId == ItemId;
		});
	}

	FMatterFluxQuestState* FindQuest(
		TArray<FMatterFluxQuestState>& Quests,
		const FName QuestId)
	{
		return Quests.FindByPredicate([QuestId](const FMatterFluxQuestState& Quest)
		{
			return Quest.QuestId == QuestId;
		});
	}

	const FMatterFluxQuestState* FindQuest(
		const TArray<FMatterFluxQuestState>& Quests,
		const FName QuestId)
	{
		return Quests.FindByPredicate([QuestId](const FMatterFluxQuestState& Quest)
		{
			return Quest.QuestId == QuestId;
		});
	}

	bool ArePrerequisitesComplete(
		const FMatterFluxQuestDefinition& Definition,
		const TArray<FMatterFluxQuestState>& Quests)
	{
		for (const FName Prerequisite : Definition.Prerequisites)
		{
			const FMatterFluxQuestState* State = FindQuest(Quests, Prerequisite);
			if (!State || State->Status != EMatterFluxQuestRuntimeStatus::Completed)
			{
				return false;
			}
		}
		return true;
	}

	void AppendRewards(
		const TArray<FMatterFluxQuestRewardDefinition>& Rewards,
		FMatterFluxProgressionEffects& Effects)
	{
		Effects.Rewards.Append(Rewards);
	}

	void ActivateQuestTree(
		const FMatterFluxContentRegistry& Registry,
		const FName QuestId,
		TArray<FMatterFluxQuestState>& Quests,
		FName& SelectedQuest,
		FMatterFluxProgressionEffects& Effects)
	{
		const FMatterFluxQuestDefinition* Definition =
			Registry.Quests.Find(QuestId);
		if (!Definition || FindQuest(Quests, QuestId))
		{
			return;
		}
		FMatterFluxQuestState& State = Quests.AddDefaulted_GetRef();
		State.QuestId = QuestId;
		State.Status = ArePrerequisitesComplete(*Definition, Quests)
			? EMatterFluxQuestRuntimeStatus::Active
			: EMatterFluxQuestRuntimeStatus::Hidden;
		if (State.Status == EMatterFluxQuestRuntimeStatus::Active)
		{
			AppendRewards(Definition->ActivationRewards, Effects);
			State.bActivationRewardsGranted = true;
			if (Definition->bFocusOnActivate
				&& Definition->Category != EMatterFluxQuestCategory::Objective)
			{
				SelectedQuest = QuestId;
			}
		}
		for (const FName ChildId : Definition->Subquests)
		{
			ActivateQuestTree(
				Registry, ChildId, Quests, SelectedQuest, Effects);
		}
	}

	bool IsEquippedObjectiveSatisfied(
		const FMatterFluxQuestDefinition& Definition,
		const FMatterFluxProgressionEvaluationContext& Context)
	{
		const int32 FirstSlot = Definition.EquipmentSlot == INDEX_NONE
			? 0 : Definition.EquipmentSlot;
		const int32 LastSlot = Definition.EquipmentSlot == INDEX_NONE
			? FMath::Max(Context.EquippedWands.Num(),
				Context.EquippedSpellsBySlot.Num()) - 1
			: Definition.EquipmentSlot;
		for (int32 Slot = FirstSlot; Slot <= LastSlot; ++Slot)
		{
			if (Definition.Objective == EMatterFluxQuestObjectiveKind::EquipWand)
			{
				const FName Equipped = Context.EquippedWands.IsValidIndex(Slot)
					? Context.EquippedWands[Slot] : NAME_None;
				if (!Equipped.IsNone()
					&& (Definition.TargetId.IsNone()
						|| Equipped == Definition.TargetId))
				{
					return true;
				}
			}
			else if (Context.EquippedSpellsBySlot.IsValidIndex(Slot))
			{
				const TSet<FName>& Spells = Context.EquippedSpellsBySlot[Slot];
				if ((Definition.TargetId.IsNone() && !Spells.IsEmpty())
					|| (!Definition.TargetId.IsNone()
						&& Spells.Contains(Definition.TargetId)))
				{
					return true;
				}
			}
		}
		return false;
	}

	bool AreRequiredChildrenComplete(
		const FMatterFluxContentRegistry& Registry,
		const FMatterFluxQuestDefinition& Definition,
		const TArray<FMatterFluxQuestState>& Quests)
	{
		for (const FName ChildId : Definition.Subquests)
		{
			const FMatterFluxQuestDefinition* ChildDefinition =
				Registry.Quests.Find(ChildId);
			if (!ChildDefinition || ChildDefinition->bOptional)
			{
				continue;
			}
			const FMatterFluxQuestState* ChildState = FindQuest(Quests, ChildId);
			if (!ChildState
				|| ChildState->Status != EMatterFluxQuestRuntimeStatus::Completed)
			{
				return false;
			}
		}
		return true;
	}

	void SortState(
		TArray<FMatterFluxItemStack>& Items,
		TArray<FMatterFluxQuestState>& Quests)
	{
		Items.Sort([](const FMatterFluxItemStack& A, const FMatterFluxItemStack& B)
		{
			return A.ItemId.LexicalLess(B.ItemId);
		});
		Quests.Sort([](const FMatterFluxQuestState& A, const FMatterFluxQuestState& B)
		{
			return A.QuestId.LexicalLess(B.QuestId);
		});
	}

	bool SameItems(
		const TArray<FMatterFluxItemStack>& A,
		const TArray<FMatterFluxItemStack>& B)
	{
		if (A.Num() != B.Num()) return false;
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (A[Index].ItemId != B[Index].ItemId
				|| A[Index].Quantity != B[Index].Quantity)
			{
				return false;
			}
		}
		return true;
	}

	bool SameQuests(
		const TArray<FMatterFluxQuestState>& A,
		const TArray<FMatterFluxQuestState>& B)
	{
		if (A.Num() != B.Num()) return false;
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (A[Index].QuestId != B[Index].QuestId
				|| A[Index].Status != B[Index].Status
				|| A[Index].Progress != B[Index].Progress
				|| A[Index].bActivationRewardsGranted
					!= B[Index].bActivationRewardsGranted
				|| A[Index].bCompletionRewardsGranted
					!= B[Index].bCompletionRewardsGranted)
			{
				return false;
			}
		}
		return true;
	}

	bool Stabilize(
		const FMatterFluxContentRegistry& Registry,
		const FMatterFluxProgressionEvaluationContext& Context,
		TArray<FMatterFluxQuestState>& Quests,
		FName& SelectedQuest,
		FMatterFluxProgressionEffects& Effects,
		FString& OutError)
	{
		TArray<FName> DefinitionIds;
		Registry.Quests.GetKeys(DefinitionIds);
		DefinitionIds.Sort(FNameLexicalLess());
		const int32 IterationLimit = FMath::Max(Registry.Quests.Num() * 3, 1);
		for (int32 Iteration = 0; Iteration < IterationLimit; ++Iteration)
		{
			bool bChanged = false;
			for (const FName QuestId : DefinitionIds)
			{
				const FMatterFluxQuestDefinition& Definition =
					Registry.Quests.FindChecked(QuestId);
				if (Definition.bStarter && !FindQuest(Quests, QuestId))
				{
					ActivateQuestTree(
						Registry, QuestId, Quests, SelectedQuest, Effects);
					bChanged = true;
				}
				if (FindQuest(Quests, QuestId))
				{
					for (const FName ChildId : Definition.Subquests)
					{
						if (!FindQuest(Quests, ChildId))
						{
							ActivateQuestTree(
								Registry, ChildId, Quests,
								SelectedQuest, Effects);
							bChanged = true;
						}
					}
				}
				if (!FindQuest(Quests, QuestId)
					&& !Definition.Prerequisites.IsEmpty()
					&& ArePrerequisitesComplete(Definition, Quests))
				{
					ActivateQuestTree(
						Registry, QuestId, Quests, SelectedQuest, Effects);
					bChanged = true;
				}
			}
			for (FMatterFluxQuestState& State : Quests)
			{
				const FMatterFluxQuestDefinition* Definition =
					Registry.Quests.Find(State.QuestId);
				if (!Definition) continue;
				if (State.Status == EMatterFluxQuestRuntimeStatus::Hidden
					&& ArePrerequisitesComplete(*Definition, Quests))
				{
					State.Status = EMatterFluxQuestRuntimeStatus::Active;
					if (!State.bActivationRewardsGranted)
					{
						AppendRewards(Definition->ActivationRewards, Effects);
						State.bActivationRewardsGranted = true;
					}
					bChanged = true;
				}
				if (State.Status != EMatterFluxQuestRuntimeStatus::Active)
				{
					continue;
				}
				bool bCompleted = false;
				switch (Definition->Objective)
				{
				case EMatterFluxQuestObjectiveKind::CompleteChildren:
					bCompleted = AreRequiredChildrenComplete(
						Registry, *Definition, Quests);
					break;
				case EMatterFluxQuestObjectiveKind::EquipWand:
				case EMatterFluxQuestObjectiveKind::EquipSpell:
					bCompleted = IsEquippedObjectiveSatisfied(*Definition, Context);
					break;
				case EMatterFluxQuestObjectiveKind::KillEnemies:
				case EMatterFluxQuestObjectiveKind::SpendItem:
					bCompleted = State.Progress >= Definition->TargetCount;
					break;
				case EMatterFluxQuestObjectiveKind::Never:
				default:
					break;
				}
				if (bCompleted)
				{
					State.Progress = FMath::Max(
						State.Progress, Definition->TargetCount);
					State.Status = EMatterFluxQuestRuntimeStatus::Completed;
					if (!State.bCompletionRewardsGranted)
					{
						AppendRewards(Definition->CompletionRewards, Effects);
						State.bCompletionRewardsGranted = true;
					}
					bChanged = true;
				}
			}
			if (!bChanged)
			{
				return true;
			}
		}
		OutError = TEXT("quest graph did not stabilize within its deterministic iteration budget");
		return false;
	}
}

bool FMatterFluxProgressionRules::BuildStarterState(
	const FMatterFluxContentRegistry& Registry,
	const FMatterFluxProgressionEvaluationContext& Context,
	TArray<FMatterFluxItemStack>& Items,
	TArray<FMatterFluxQuestState>& Quests,
	FName& SelectedQuest,
	FMatterFluxProgressionEffects& OutEffects,
	FString& OutError)
{
	using namespace MatterFluxProgressionRules;
	OutError.Reset();
	OutEffects.Reset();
	Items.Reset();
	Quests.Reset();
	SelectedQuest = NAME_None;
	TArray<FName> ItemIds;
	Registry.Items.GetKeys(ItemIds);
	ItemIds.Sort(FNameLexicalLess());
	for (const FName ItemId : ItemIds)
	{
		const FMatterFluxItemDefinition& Definition =
			Registry.Items.FindChecked(ItemId);
		if (Definition.StarterCount > 0)
		{
			FMatterFluxItemStack& Stack = Items.AddDefaulted_GetRef();
			Stack.ItemId = ItemId;
			Stack.Quantity = Definition.StarterCount;
		}
	}
	TArray<FName> QuestIds;
	Registry.Quests.GetKeys(QuestIds);
	QuestIds.Sort(FNameLexicalLess());
	for (const FName QuestId : QuestIds)
	{
		if (Registry.Quests.FindChecked(QuestId).bStarter)
		{
			ActivateQuestTree(
				Registry, QuestId, Quests, SelectedQuest, OutEffects);
		}
	}
	if (!Stabilize(
		Registry, Context, Quests, SelectedQuest, OutEffects, OutError))
	{
		Items.Reset();
		Quests.Reset();
		SelectedQuest = NAME_None;
		OutEffects.Reset();
		return false;
	}
	SortState(Items, Quests);
	return true;
}

bool FMatterFluxProgressionRules::AddItem(
	const FMatterFluxContentRegistry& Registry,
	TArray<FMatterFluxItemStack>& Items,
	const FName ItemId,
	const int32 Delta,
	int32& OutPreviousQuantity,
	int32& OutNewQuantity,
	FString& OutError)
{
	using namespace MatterFluxProgressionRules;
	OutError.Reset();
	if (Delta == 0)
	{
		OutError = TEXT("item quantity delta must be non-zero");
		return false;
	}
	const FMatterFluxItemDefinition* Definition = Registry.Items.Find(ItemId);
	if (!Definition)
	{
		OutError = FString::Printf(TEXT("unknown item '%s'"), *ItemId.ToString());
		return false;
	}
	FMatterFluxItemStack* Existing = FindItem(Items, ItemId);
	OutPreviousQuantity = Existing ? Existing->Quantity : 0;
	const int64 NextQuantity = static_cast<int64>(OutPreviousQuantity) + Delta;
	if (NextQuantity < 0 || NextQuantity > Definition->MaxStack)
	{
		OutError = FString::Printf(
			TEXT("item '%s' quantity would be outside [0,%d]"),
			*ItemId.ToString(), Definition->MaxStack);
		return false;
	}
	OutNewQuantity = static_cast<int32>(NextQuantity);
	if (OutNewQuantity == 0)
	{
		Items.RemoveAll([ItemId](const FMatterFluxItemStack& Item)
		{
			return Item.ItemId == ItemId;
		});
	}
	else if (Existing)
	{
		Existing->Quantity = OutNewQuantity;
	}
	else
	{
		FMatterFluxItemStack& Added = Items.AddDefaulted_GetRef();
		Added.ItemId = ItemId;
		Added.Quantity = OutNewQuantity;
	}
	Items.Sort([](const FMatterFluxItemStack& A, const FMatterFluxItemStack& B)
	{
		return A.ItemId.LexicalLess(B.ItemId);
	});
	return true;
}

bool FMatterFluxProgressionRules::UseItem(
	const FMatterFluxContentRegistry& Registry,
	TArray<FMatterFluxItemStack>& Items,
	const FName ItemId,
	FMatterFluxProgressionEffects& OutEffects,
	FString& OutError)
{
	using namespace MatterFluxProgressionRules;
	OutEffects.Reset();
	OutError.Reset();
	const FMatterFluxItemDefinition* Definition = Registry.Items.Find(ItemId);
	const FMatterFluxItemStack* Stack = FindItem(Items, ItemId);
	if (!Definition)
	{
		OutError = FString::Printf(TEXT("unknown item '%s'"), *ItemId.ToString());
		return false;
	}
	if (Definition->UseAction == EMatterFluxItemUseAction::None)
	{
		OutError = FString::Printf(TEXT("item '%s' is not usable"), *ItemId.ToString());
		return false;
	}
	if (!Stack || Stack->Quantity < Definition->ConsumeCount)
	{
		OutError = FString::Printf(
			TEXT("item '%s' has insufficient quantity"), *ItemId.ToString());
		return false;
	}
	TArray<FMatterFluxItemStack> Candidate = Items;
	int32 Previous = 0;
	int32 Current = 0;
	if (!AddItem(
		Registry, Candidate, ItemId, -Definition->ConsumeCount,
		Previous, Current, OutError))
	{
		return false;
	}
	Items = MoveTemp(Candidate);
	OutEffects.ItemUseAction = Definition->UseAction;
	OutEffects.ItemUseMagnitude = Definition->UseMagnitude;
	OutEffects.GameplayEventTag = Definition->GameplayEventTag;
	return true;
}

bool FMatterFluxProgressionRules::NotifyEvent(
	const FMatterFluxContentRegistry& Registry,
	const FMatterFluxProgressionEvaluationContext& Context,
	TArray<FMatterFluxItemStack>& Items,
	TArray<FMatterFluxQuestState>& Quests,
	FName& SelectedQuest,
	const FMatterFluxQuestEvent& Event,
	FMatterFluxProgressionEffects& OutEffects,
	FString& OutError)
{
	using namespace MatterFluxProgressionRules;
	OutEffects.Reset();
	OutError.Reset();
	TArray<FMatterFluxQuestState> Candidate = Quests;
	FName CandidateSelection = SelectedQuest;
	for (FMatterFluxQuestState& State : Candidate)
	{
		if (State.Status != EMatterFluxQuestRuntimeStatus::Active)
		{
			continue;
		}
		const FMatterFluxQuestDefinition* Definition =
			Registry.Quests.Find(State.QuestId);
		if (!Definition) continue;
		int32 Increment = 0;
		if (Definition->Objective == EMatterFluxQuestObjectiveKind::KillEnemies
			&& Event.Type == EMatterFluxQuestEventType::EnemyKilled
			&& (Definition->TargetId.IsNone()
				|| Definition->TargetId == Event.SubjectId)
			&& (Definition->TargetLevel == INDEX_NONE
				|| Definition->TargetLevel == Event.SubjectLevel))
		{
			Increment = FMath::Max(Event.Amount, 0);
		}
		else if (Definition->Objective == EMatterFluxQuestObjectiveKind::SpendItem
			&& Event.Type == EMatterFluxQuestEventType::ItemChanged
			&& Definition->TargetId == Event.SubjectId)
		{
			Increment = FMath::Max(
				Event.PreviousItemCount - Event.NewItemCount, 0);
		}
		if (Increment > 0)
		{
			State.Progress = FMath::Clamp(
				State.Progress + Increment, 0, Definition->TargetCount);
		}
	}
	if (!Stabilize(
		Registry, Context, Candidate, CandidateSelection,
		OutEffects, OutError))
	{
		OutEffects.Reset();
		return false;
	}
	Quests = MoveTemp(Candidate);
	SelectedQuest = CandidateSelection;
	SortState(Items, Quests);
	return true;
}

UMatterFluxProgressionComponent::UMatterFluxProgressionComponent()
{
	SetIsReplicatedByDefault(true);
}

void UMatterFluxProgressionComponent::BeginPlay()
{
	Super::BeginPlay();
	ItemStacks.Owner = this;
	QuestStates.Owner = this;
	ContentReloadedHandle =
		IMatterFluxScriptRuntime::Get().OnContentReloaded().AddUObject(
			this, &UMatterFluxProgressionComponent::HandleContentReloaded);
	if (UMatterFluxMagicInventoryComponent* Inventory = ResolveMagicInventory())
	{
		BoundMagicInventory = Inventory;
		LastEvaluatedMagicInventoryRevision =
			Inventory->GetInventoryRevision();
		MagicInventoryChangedHandle = Inventory->OnInventoryChanged().AddUObject(
			this, &UMatterFluxProgressionComponent::HandleMagicInventoryChanged);
	}
	if (GetOwner() && GetOwner()->HasAuthority() && Revision == 0)
	{
		FString Error;
		if (!ResetToStarterStateAuthority(Error) && !Error.IsEmpty())
		{
			UE_LOG(LogMatterFlux, Verbose,
				TEXT("Progression initialization deferred: %s"), *Error);
		}
	}
}

void UMatterFluxProgressionComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	if (UMatterFluxMagicInventoryComponent* Inventory = BoundMagicInventory.Get();
		Inventory && MagicInventoryChangedHandle.IsValid())
	{
		Inventory->OnInventoryChanged().Remove(MagicInventoryChangedHandle);
	}
	MagicInventoryChangedHandle.Reset();
	BoundMagicInventory.Reset();
	if (ContentReloadedHandle.IsValid()
		&& IMatterFluxScriptRuntime::IsAvailable())
	{
		IMatterFluxScriptRuntime::Get().OnContentReloaded().Remove(
			ContentReloadedHandle);
	}
	ContentReloadedHandle.Reset();
	Super::EndPlay(EndPlayReason);
}

void UMatterFluxProgressionComponent::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(
		UMatterFluxProgressionComponent, ItemStacks, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(
		UMatterFluxProgressionComponent, QuestStates, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(
		UMatterFluxProgressionComponent, SelectedQuest, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(
		UMatterFluxProgressionComponent, Revision, COND_OwnerOnly);
}

int32 UMatterFluxProgressionComponent::GetItemQuantity(FName ItemId) const
{
	const FMatterFluxItemStack* Stack = ItemStacks.Items.FindByPredicate(
		[ItemId](const FMatterFluxItemStack& Item) { return Item.ItemId == ItemId; });
	return Stack ? Stack->Quantity : 0;
}

const FMatterFluxQuestState* UMatterFluxProgressionComponent::FindQuestState(
	FName QuestId) const
{
	return QuestStates.Items.FindByPredicate(
		[QuestId](const FMatterFluxQuestState& Quest) { return Quest.QuestId == QuestId; });
}

void UMatterFluxProgressionComponent::RequestUseItem(const FName ItemId)
{
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		FString Error;
		if (!UseItemAuthority(ItemId, Revision, Error))
		{
			UE_LOG(LogMatterFlux, Warning, TEXT("Rejected item use: %s"), *Error);
		}
	}
	else
	{
		ServerUseItem(ItemId, Revision);
	}
}

void UMatterFluxProgressionComponent::RequestSelectQuest(const FName QuestId)
{
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		FString Error;
		SelectQuestAuthority(QuestId, Error);
	}
	else
	{
		ServerSelectQuest(QuestId);
	}
}

bool UMatterFluxProgressionComponent::AddItemAuthority(
	const FName ItemId, const int32 Delta, FString& OutError)
{
	OutError.Reset();
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		OutError = TEXT("item changes require authority");
		return false;
	}
	if (bApplyingEffects)
	{
		OutError = TEXT("item changes cannot re-enter progression effects");
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!Registry.IsValid())
	{
		OutError = TEXT("progression content registry is unavailable");
		return false;
	}
	TArray<FMatterFluxItemStack> NextItems = ItemStacks.Items;
	TArray<FMatterFluxQuestState> NextQuests = QuestStates.Items;
	FName NextSelection = SelectedQuest;
	int32 Previous = 0;
	int32 Current = 0;
	if (!FMatterFluxProgressionRules::AddItem(
		*Registry, NextItems, ItemId, Delta, Previous, Current, OutError))
	{
		return false;
	}
	FMatterFluxQuestEvent Event;
	Event.Type = EMatterFluxQuestEventType::ItemChanged;
	Event.SubjectId = ItemId;
	Event.PreviousItemCount = Previous;
	Event.NewItemCount = Current;
	FMatterFluxProgressionEffects Effects;
	if (!FMatterFluxProgressionRules::NotifyEvent(
		*Registry, BuildEvaluationContext(), NextItems, NextQuests,
		NextSelection, Event, Effects, OutError))
	{
		return false;
	}
	for (const FMatterFluxQuestRewardDefinition& Reward : Effects.Rewards)
	{
		if (Reward.Kind == EMatterFluxQuestRewardKind::Item)
		{
			int32 IgnoredPrevious = 0;
			int32 IgnoredCurrent = 0;
			if (!FMatterFluxProgressionRules::AddItem(
				*Registry, NextItems, Reward.ContentId, Reward.Quantity,
				IgnoredPrevious, IgnoredCurrent, OutError))
			{
				return false;
			}
		}
	}
	if (!ApplyEffectsAuthority(Effects, OutError))
	{
		return false;
	}
	ItemStacks.Items = MoveTemp(NextItems);
	QuestStates.Items = MoveTemp(NextQuests);
	SelectedQuest = NextSelection;
	++Revision;
	MarkAllDirty();
	ProgressionChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
	return true;
}

bool UMatterFluxProgressionComponent::PurchaseOfferAuthority(
	const FMatterFluxShopOfferDefinition& Offer,
	const FName OfferKey,
	const int32 ExpectedRevision,
	int32& OutRemainingPurchases,
	FString& OutError)
{
	OutRemainingPurchases = INDEX_NONE;
	OutError.Reset();
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		OutError = TEXT("shop purchases require authority");
		return false;
	}
	if (bApplyingEffects)
	{
		OutError = TEXT("shop purchases cannot re-enter progression effects");
		return false;
	}
	if (ExpectedRevision != Revision)
	{
		OutError = FString::Printf(
			TEXT("stale progression revision %d; expected %d"),
			ExpectedRevision, Revision);
		return false;
	}
	if (OfferKey.IsNone() || Offer.ProductId.IsNone() || Offer.ProductCount <= 0
		|| Offer.CostItemId.IsNone() || Offer.CostCount <= 0)
	{
		OutError = TEXT("shop offer is invalid");
		return false;
	}
	const int32 PreviousPurchaseCount = ShopPurchaseCounts.FindRef(OfferKey);
	if (Offer.PurchaseLimit >= 0
		&& PreviousPurchaseCount >= Offer.PurchaseLimit)
	{
		OutRemainingPurchases = 0;
		OutError = TEXT("this offer has reached its purchase limit");
		return false;
	}

	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!Registry.IsValid())
	{
		OutError = TEXT("progression content registry is unavailable");
		return false;
	}

	TArray<FMatterFluxItemStack> NextItems = ItemStacks.Items;
	TArray<FMatterFluxQuestState> NextQuests = QuestStates.Items;
	FName NextSelection = SelectedQuest;
	int32 PreviousCostCount = 0;
	int32 NewCostCount = 0;
	if (!FMatterFluxProgressionRules::AddItem(
		*Registry, NextItems, Offer.CostItemId, -Offer.CostCount,
		PreviousCostCount, NewCostCount, OutError))
	{
		return false;
	}

	FMatterFluxProgressionEffects Effects;
	if (Offer.ProductKind == EMatterFluxShopProductKind::Item)
	{
		int32 IgnoredPrevious = 0;
		int32 IgnoredCurrent = 0;
		if (!FMatterFluxProgressionRules::AddItem(
			*Registry, NextItems, Offer.ProductId, Offer.ProductCount,
			IgnoredPrevious, IgnoredCurrent, OutError))
		{
			return false;
		}
	}
	else
	{
		FMatterFluxQuestRewardDefinition ProductReward;
		ProductReward.Kind = Offer.ProductKind == EMatterFluxShopProductKind::Spell
			? EMatterFluxQuestRewardKind::Spell
			: EMatterFluxQuestRewardKind::Wand;
		ProductReward.ContentId = Offer.ProductId;
		ProductReward.Quantity = Offer.ProductCount;
		Effects.Rewards.Add(ProductReward);
	}

	FMatterFluxQuestEvent PurchaseEvent;
	PurchaseEvent.Type = EMatterFluxQuestEventType::ItemChanged;
	PurchaseEvent.SubjectId = Offer.CostItemId;
	PurchaseEvent.PreviousItemCount = PreviousCostCount;
	PurchaseEvent.NewItemCount = NewCostCount;
	FMatterFluxProgressionEffects QuestEffects;
	if (!FMatterFluxProgressionRules::NotifyEvent(
		*Registry, BuildEvaluationContext(), NextItems, NextQuests,
		NextSelection, PurchaseEvent, QuestEffects, OutError))
	{
		return false;
	}
	Effects.Rewards.Append(QuestEffects.Rewards);
	for (const FMatterFluxQuestRewardDefinition& Reward : Effects.Rewards)
	{
		if (Reward.Kind != EMatterFluxQuestRewardKind::Item) continue;
		int32 IgnoredPrevious = 0;
		int32 IgnoredCurrent = 0;
		if (!FMatterFluxProgressionRules::AddItem(
			*Registry, NextItems, Reward.ContentId, Reward.Quantity,
			IgnoredPrevious, IgnoredCurrent, OutError))
		{
			return false;
		}
	}

	if (!ApplyEffectsAuthority(Effects, OutError))
	{
		return false;
	}
	ItemStacks.Items = MoveTemp(NextItems);
	QuestStates.Items = MoveTemp(NextQuests);
	SelectedQuest = NextSelection;
	ShopPurchaseCounts.Add(OfferKey, PreviousPurchaseCount + 1);
	OutRemainingPurchases = Offer.PurchaseLimit >= 0
		? Offer.PurchaseLimit - PreviousPurchaseCount - 1
		: INDEX_NONE;
	++Revision;
	MarkAllDirty();
	ProgressionChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
	return true;
}

bool UMatterFluxProgressionComponent::UseItemAuthority(
	const FName ItemId, const int32 ExpectedRevision, FString& OutError)
{
	OutError.Reset();
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		OutError = TEXT("item use requires authority");
		return false;
	}
	if (bApplyingEffects)
	{
		OutError = TEXT("item use cannot re-enter progression effects");
		return false;
	}
	if (ExpectedRevision != Revision)
	{
		OutError = FString::Printf(
			TEXT("stale progression revision %d; expected %d"),
			ExpectedRevision, Revision);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!Registry.IsValid())
	{
		OutError = TEXT("progression content registry is unavailable");
		return false;
	}
	TArray<FMatterFluxItemStack> NextItems = ItemStacks.Items;
	TArray<FMatterFluxQuestState> NextQuests = QuestStates.Items;
	FName NextSelection = SelectedQuest;
	const int32 Previous = GetItemQuantity(ItemId);
	FMatterFluxProgressionEffects Effects;
	if (!FMatterFluxProgressionRules::UseItem(
		*Registry, NextItems, ItemId, Effects, OutError))
	{
		return false;
	}
	const EMatterFluxItemUseAction UseAction = Effects.ItemUseAction;
	const float UseMagnitude = Effects.ItemUseMagnitude;
	const FName GameplayEventTag = Effects.GameplayEventTag;
	FMatterFluxQuestEvent Event;
	Event.Type = EMatterFluxQuestEventType::ItemChanged;
	Event.SubjectId = ItemId;
	Event.PreviousItemCount = Previous;
	const FMatterFluxItemStack* Remaining = NextItems.FindByPredicate(
		[ItemId](const FMatterFluxItemStack& Stack) { return Stack.ItemId == ItemId; });
	Event.NewItemCount = Remaining ? Remaining->Quantity : 0;
	FMatterFluxProgressionEffects QuestEffects;
	if (!FMatterFluxProgressionRules::NotifyEvent(
		*Registry, BuildEvaluationContext(), NextItems, NextQuests,
		NextSelection, Event, QuestEffects, OutError))
	{
		return false;
	}
	Effects.Rewards.Append(QuestEffects.Rewards);
	Effects.ItemUseAction = UseAction;
	Effects.ItemUseMagnitude = UseMagnitude;
	Effects.GameplayEventTag = GameplayEventTag;
	for (const FMatterFluxQuestRewardDefinition& Reward : Effects.Rewards)
	{
		if (Reward.Kind == EMatterFluxQuestRewardKind::Item)
		{
			int32 IgnoredPrevious = 0;
			int32 IgnoredCurrent = 0;
			if (!FMatterFluxProgressionRules::AddItem(
				*Registry, NextItems, Reward.ContentId, Reward.Quantity,
				IgnoredPrevious, IgnoredCurrent, OutError))
			{
				return false;
			}
		}
	}
	if (!ApplyEffectsAuthority(Effects, OutError))
	{
		return false;
	}
	ItemStacks.Items = MoveTemp(NextItems);
	QuestStates.Items = MoveTemp(NextQuests);
	SelectedQuest = NextSelection;
	++Revision;
	MarkAllDirty();
	ProgressionChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
	return true;
}

bool UMatterFluxProgressionComponent::SelectQuestAuthority(
	const FName QuestId, FString& OutError)
{
	OutError.Reset();
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		OutError = TEXT("quest selection requires authority");
		return false;
	}
	if (bApplyingEffects)
	{
		OutError = TEXT("quest selection cannot re-enter progression effects");
		return false;
	}
	const FMatterFluxQuestState* State = FindQuestState(QuestId);
	if (!State || State->Status == EMatterFluxQuestRuntimeStatus::Hidden
		|| State->Status == EMatterFluxQuestRuntimeStatus::Failed)
	{
		OutError = TEXT("only a visible quest can be selected");
		return false;
	}
	if (SelectedQuest == QuestId)
	{
		return true;
	}
	SelectedQuest = QuestId;
	++Revision;
	ProgressionChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
	return true;
}
bool UMatterFluxProgressionComponent::NotifyQuestEventAuthority(
	const FMatterFluxQuestEvent& Event, FString& OutError)
{
	OutError.Reset();
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		OutError = TEXT("quest events require authority");
		return false;
	}
	if (bApplyingEffects)
	{
		OutError = TEXT("quest events cannot re-enter progression effects");
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!Registry.IsValid())
	{
		OutError = TEXT("progression content registry is unavailable");
		return false;
	}
	TArray<FMatterFluxItemStack> NextItems = ItemStacks.Items;
	TArray<FMatterFluxQuestState> NextQuests = QuestStates.Items;
	FName NextSelection = SelectedQuest;
	FMatterFluxProgressionEffects Effects;
	if (!FMatterFluxProgressionRules::NotifyEvent(
		*Registry, BuildEvaluationContext(), NextItems, NextQuests,
		NextSelection, Event, Effects, OutError))
	{
		return false;
	}
	for (const FMatterFluxQuestRewardDefinition& Reward : Effects.Rewards)
	{
		if (Reward.Kind == EMatterFluxQuestRewardKind::Item)
		{
			int32 Previous = 0;
			int32 Current = 0;
			if (!FMatterFluxProgressionRules::AddItem(
				*Registry, NextItems, Reward.ContentId, Reward.Quantity,
				Previous, Current, OutError))
			{
				return false;
			}
		}
	}
	const bool bItemsChanged = !MatterFluxProgressionRules::SameItems(
		NextItems, ItemStacks.Items);
	const bool bQuestsChanged = !MatterFluxProgressionRules::SameQuests(
		NextQuests, QuestStates.Items);
	if (!bItemsChanged && !bQuestsChanged && NextSelection == SelectedQuest
		&& Effects.Rewards.IsEmpty())
	{
		return true;
	}
	if (!ApplyEffectsAuthority(Effects, OutError))
	{
		return false;
	}
	ItemStacks.Items = MoveTemp(NextItems);
	QuestStates.Items = MoveTemp(NextQuests);
	SelectedQuest = NextSelection;
	++Revision;
	MarkAllDirty();
	ProgressionChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
	return true;
}

bool UMatterFluxProgressionComponent::ResetToStarterStateAuthority(
	FString& OutError)
{
	OutError.Reset();
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		OutError = TEXT("progression reset requires authority");
		return false;
	}
	if (bApplyingEffects)
	{
		OutError = TEXT("progression reset cannot re-enter progression effects");
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!Registry.IsValid())
	{
		OutError = TEXT("progression content registry is unavailable");
		return false;
	}
	TArray<FMatterFluxItemStack> NextItems;
	TArray<FMatterFluxQuestState> NextQuests;
	FName NextSelection;
	FMatterFluxProgressionEffects Effects;
	if (!FMatterFluxProgressionRules::BuildStarterState(
		*Registry, BuildEvaluationContext(), NextItems, NextQuests,
		NextSelection, Effects, OutError))
	{
		return false;
	}
	for (const FMatterFluxQuestRewardDefinition& Reward : Effects.Rewards)
	{
		if (Reward.Kind == EMatterFluxQuestRewardKind::Item)
		{
			int32 Previous = 0;
			int32 Current = 0;
			if (!FMatterFluxProgressionRules::AddItem(
				*Registry, NextItems, Reward.ContentId, Reward.Quantity,
				Previous, Current, OutError))
			{
				return false;
			}
		}
	}
	if (!ApplyEffectsAuthority(Effects, OutError))
	{
		return false;
	}
	ItemStacks.Items = MoveTemp(NextItems);
	QuestStates.Items = MoveTemp(NextQuests);
	SelectedQuest = NextSelection;
	ShopPurchaseCounts.Reset();
	Revision = FMath::Max(Revision + 1, 1);
	MarkAllDirty();
	ProgressionChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
	return true;
}
bool UMatterFluxProgressionComponent::CaptureSaveState(
	FMatterFluxProgressionSaveState& OutState, FString& OutError) const
{
	OutState = FMatterFluxProgressionSaveState();
	OutError.Reset();
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		OutError = TEXT("progression save capture requires authority");
		return false;
	}
	OutState.Items.Reserve(ItemStacks.Items.Num());
	for (const FMatterFluxItemStack& Stack : ItemStacks.Items)
	{
		FMatterFluxSavedItemStack& Saved = OutState.Items.AddDefaulted_GetRef();
		Saved.ItemId = Stack.ItemId;
		Saved.Quantity = Stack.Quantity;
	}
	OutState.Quests.Reserve(QuestStates.Items.Num());
	for (const FMatterFluxQuestState& State : QuestStates.Items)
	{
		FMatterFluxSavedQuestState& Saved = OutState.Quests.AddDefaulted_GetRef();
		Saved.QuestId = State.QuestId;
		Saved.Status = static_cast<uint8>(State.Status);
		Saved.Progress = State.Progress;
		Saved.bActivationRewardsGranted = State.bActivationRewardsGranted;
		Saved.bCompletionRewardsGranted = State.bCompletionRewardsGranted;
	}
	OutState.SelectedQuest = SelectedQuest;
	TArray<FName> PurchaseKeys;
	ShopPurchaseCounts.GenerateKeyArray(PurchaseKeys);
	PurchaseKeys.Sort(FNameLexicalLess());
	for (const FName OfferKey : PurchaseKeys)
	{
		FMatterFluxSavedShopPurchase& Saved =
			OutState.ShopPurchases.AddDefaulted_GetRef();
		Saved.OfferKey = OfferKey;
		Saved.PurchaseCount = ShopPurchaseCounts.FindChecked(OfferKey);
	}
	OutState.Revision = FMath::Max(Revision, 1);
	return true;
}

bool UMatterFluxProgressionComponent::RestoreSaveStateAuthority(
	const FMatterFluxProgressionSaveState& State, FString& OutError)
{
	OutError.Reset();
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		OutError = TEXT("progression save restore requires authority");
		return false;
	}
	if (bApplyingEffects)
	{
		OutError = TEXT("progression restore cannot re-enter progression effects");
		return false;
	}
	if (State.Revision == 0 && State.Items.IsEmpty()
		&& State.Quests.IsEmpty() && State.ShopPurchases.IsEmpty())
	{
		return ResetToStarterStateAuthority(OutError);
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!Registry.IsValid() || State.Revision < 0
		|| (State.Revision == 0
			&& (!State.Items.IsEmpty() || !State.Quests.IsEmpty()
				|| !State.ShopPurchases.IsEmpty()
				|| !State.SelectedQuest.IsNone()))
		|| State.Items.Num() > 512 || State.Quests.Num() > 512
		|| State.ShopPurchases.Num() > 512)
	{
		OutError = TEXT("saved progression metadata is invalid");
		return false;
	}
	TArray<FMatterFluxItemStack> CandidateItems;
	TSet<FName> SeenItems;
	for (const FMatterFluxSavedItemStack& Saved : State.Items)
	{
		const FMatterFluxItemDefinition* Definition = Registry->Items.Find(Saved.ItemId);
		if (!Definition || Saved.Quantity <= 0
			|| Saved.Quantity > Definition->MaxStack || SeenItems.Contains(Saved.ItemId))
		{
			OutError = TEXT("saved progression contains an invalid item stack");
			return false;
		}
		SeenItems.Add(Saved.ItemId);
		FMatterFluxItemStack& Stack = CandidateItems.AddDefaulted_GetRef();
		Stack.ItemId = Saved.ItemId;
		Stack.Quantity = Saved.Quantity;
	}
	TArray<FMatterFluxQuestState> CandidateQuests;
	TSet<FName> SeenQuests;
	for (const FMatterFluxSavedQuestState& Saved : State.Quests)
	{
		const FMatterFluxQuestDefinition* Definition = Registry->Quests.Find(Saved.QuestId);
		if (!Definition || SeenQuests.Contains(Saved.QuestId)
			|| Saved.Status > static_cast<uint8>(EMatterFluxQuestRuntimeStatus::Failed)
			|| Saved.Progress < 0 || Saved.Progress > Definition->TargetCount)
		{
			OutError = TEXT("saved progression contains an invalid quest state");
			return false;
		}
		SeenQuests.Add(Saved.QuestId);
		FMatterFluxQuestState& Quest = CandidateQuests.AddDefaulted_GetRef();
		Quest.QuestId = Saved.QuestId;
		Quest.Status = static_cast<EMatterFluxQuestRuntimeStatus>(Saved.Status);
		Quest.Progress = Saved.Progress;
		Quest.bActivationRewardsGranted = Saved.bActivationRewardsGranted;
		Quest.bCompletionRewardsGranted = Saved.bCompletionRewardsGranted;
	}
	TMap<FName, int32> CandidateShopPurchases;
	for (const FMatterFluxSavedShopPurchase& Saved : State.ShopPurchases)
	{
		if (Saved.OfferKey.IsNone() || Saved.PurchaseCount <= 0
			|| CandidateShopPurchases.Contains(Saved.OfferKey))
		{
			OutError = TEXT("saved progression contains an invalid shop purchase");
			return false;
		}
		CandidateShopPurchases.Add(Saved.OfferKey, Saved.PurchaseCount);
	}
	if (!State.SelectedQuest.IsNone())
	{
		const FMatterFluxQuestState* Selected = CandidateQuests.FindByPredicate(
			[&State](const FMatterFluxQuestState& Quest)
			{
				return Quest.QuestId == State.SelectedQuest;
			});
		if (!Selected || Selected->Status == EMatterFluxQuestRuntimeStatus::Hidden
			|| Selected->Status == EMatterFluxQuestRuntimeStatus::Failed)
		{
			OutError = TEXT("saved progression selects an invisible quest");
			return false;
		}
	}
	CandidateItems.Sort([](const FMatterFluxItemStack& A, const FMatterFluxItemStack& B)
	{
		return A.ItemId.LexicalLess(B.ItemId);
	});
	CandidateQuests.Sort([](const FMatterFluxQuestState& A, const FMatterFluxQuestState& B)
	{
		return A.QuestId.LexicalLess(B.QuestId);
	});
	ItemStacks.Items = MoveTemp(CandidateItems);
	QuestStates.Items = MoveTemp(CandidateQuests);
	ShopPurchaseCounts = MoveTemp(CandidateShopPurchases);
	SelectedQuest = State.SelectedQuest;
	Revision = FMath::Max(State.Revision, 1);
	ItemStacks.Owner = this;
	QuestStates.Owner = this;
	MarkAllDirty();
	ProgressionChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
	return true;
}
void UMatterFluxProgressionComponent::HandleReplicatedProgressionChanged()
{
	ProgressionChanged.Broadcast();
}
FMatterFluxProgressionEvaluationContext
UMatterFluxProgressionComponent::BuildEvaluationContext() const
{
	FMatterFluxProgressionEvaluationContext Context;
	const UMatterFluxMagicInventoryComponent* Inventory = ResolveMagicInventory();
	if (!Inventory)
	{
		return Context;
	}
	const TArray<FGuid>& Equipped = Inventory->GetEquippedWands();
	Context.EquippedWands.SetNum(Equipped.Num());
	Context.EquippedSpellsBySlot.SetNum(Equipped.Num());
	for (int32 Slot = 0; Slot < Equipped.Num(); ++Slot)
	{
		const FMatterFluxOwnedWand* Wand = Inventory->FindWand(Equipped[Slot]);
		if (!Wand)
		{
			continue;
		}
		Context.EquippedWands[Slot] = Wand->DefinitionId;
		for (const FName SpellId : Wand->SpellSlots)
		{
			if (!SpellId.IsNone())
			{
				Context.EquippedSpellsBySlot[Slot].Add(SpellId);
			}
		}
	}
	return Context;
}

UMatterFluxMagicInventoryComponent*
UMatterFluxProgressionComponent::ResolveMagicInventory() const
{
	if (const AMatterFluxPlayerState* PlayerState =
		Cast<AMatterFluxPlayerState>(GetOwner()))
	{
		return PlayerState->GetMagicInventory();
	}
	return GetOwner()
		? GetOwner()->FindComponentByClass<UMatterFluxMagicInventoryComponent>()
		: nullptr;
}

bool UMatterFluxProgressionComponent::ApplyEffectsAuthority(
	const FMatterFluxProgressionEffects& Effects, FString& OutError)
{
	OutError.Reset();
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		OutError = TEXT("progression effects require authority");
		return false;
	}
	if (bApplyingEffects)
	{
		OutError = TEXT("progression effects cannot be applied recursively");
		return false;
	}
	TGuardValue<bool> ApplyingGuard(bApplyingEffects, true);

	TArray<FMatterFluxQuestRewardDefinition> MagicRewards;
	for (const FMatterFluxQuestRewardDefinition& Reward : Effects.Rewards)
	{
		if (Reward.Kind != EMatterFluxQuestRewardKind::Item)
		{
			MagicRewards.Add(Reward);
		}
	}

	AMatterFluxPlayerState* PlayerState = Cast<AMatterFluxPlayerState>(GetOwner());
	UAbilitySystemComponent* ASC = PlayerState
		? PlayerState->GetAbilitySystemComponent() : nullptr;
	UMatterFluxPlayerAttributeSet* Attributes = PlayerState
		? PlayerState->GetPlayerAttributes() : nullptr;
	FGameplayTag GameplayEventTag;
	AActor* GameplayEventTarget = nullptr;
	float WandManaRestoreAmount = 0.0f;
	switch (Effects.ItemUseAction)
	{
	case EMatterFluxItemUseAction::None:
		break;
	case EMatterFluxItemUseAction::RestoreHealth:
		if (!ASC || !Attributes || Effects.ItemUseMagnitude <= 0.0f
			|| Attributes->GetHealth() >= Attributes->GetMaxHealth())
		{
			OutError = TEXT("player cannot receive healing");
			return false;
		}
		break;
	case EMatterFluxItemUseAction::RestoreWandMana:
		WandManaRestoreAmount = Effects.ItemUseMagnitude;
		break;
	case EMatterFluxItemUseAction::GameplayEvent:
		GameplayEventTag = FGameplayTag::RequestGameplayTag(
			Effects.GameplayEventTag, false);
		GameplayEventTarget = ASC && ASC->GetAvatarActor()
			? ASC->GetAvatarActor() : GetOwner();
		if (!GameplayEventTag.IsValid() || !GameplayEventTarget)
		{
			OutError = TEXT("item gameplay event is not registered or has no target");
			return false;
		}
		break;
	default:
		OutError = TEXT("item use action is invalid");
		return false;
	}

	if (!MagicRewards.IsEmpty() || WandManaRestoreAmount > 0.0f)
	{
		UMatterFluxMagicInventoryComponent* Inventory = ResolveMagicInventory();
		if (!Inventory
			|| !Inventory->ApplyProgressionEffectsAuthority(
				MagicRewards, WandManaRestoreAmount, OutError))
		{
			return false;
		}
	}

	if (Effects.ItemUseAction == EMatterFluxItemUseAction::RestoreHealth)
	{
		ASC->SetNumericAttributeBase(
			UMatterFluxPlayerAttributeSet::GetHealthAttribute(),
			FMath::Min(Attributes->GetHealth() + Effects.ItemUseMagnitude,
				Attributes->GetMaxHealth()));
	}
	else if (Effects.ItemUseAction == EMatterFluxItemUseAction::GameplayEvent)
	{
		FGameplayEventData Payload;
		Payload.EventTag = GameplayEventTag;
		Payload.EventMagnitude = Effects.ItemUseMagnitude;
		UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(
			GameplayEventTarget, GameplayEventTag, Payload);
	}
	return true;
}

void UMatterFluxProgressionComponent::HandleMagicInventoryChanged()
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}
	if (bApplyingEffects)
	{
		if (!bMagicInventoryRefreshQueued && GetWorld())
		{
			bMagicInventoryRefreshQueued = true;
			const TWeakObjectPtr<UMatterFluxProgressionComponent> WeakThis(this);
			GetWorld()->GetTimerManager().SetTimerForNextTick(
				[WeakThis]()
				{
					if (UMatterFluxProgressionComponent* Component = WeakThis.Get())
					{
						Component->bMagicInventoryRefreshQueued = false;
						Component->HandleMagicInventoryChanged();
					}
				});
		}
		return;
	}
	const UMatterFluxMagicInventoryComponent* Inventory =
		ResolveMagicInventory();
	if (!Inventory
		|| Inventory->GetInventoryRevision()
			== LastEvaluatedMagicInventoryRevision)
	{
		// Mana regeneration and cast runtime state share the UI delegate, but
		// equipment quests only care about structural inventory revisions.
		return;
	}
	LastEvaluatedMagicInventoryRevision = Inventory->GetInventoryRevision();
	FMatterFluxQuestEvent Event;
	Event.Type = EMatterFluxQuestEventType::Refresh;
	FString Error;
	if (!NotifyQuestEventAuthority(Event, Error))
	{
		UE_LOG(LogMatterFlux, Warning,
			TEXT("Could not refresh equipment quests: %s"), *Error);
	}
}

void UMatterFluxProgressionComponent::HandleContentReloaded(
	const FMatterFluxContentRegistryPtr Registry)
{
	if (!Registry.IsValid() || !GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}
	if (!BoundMagicInventory.IsValid())
	{
		if (UMatterFluxMagicInventoryComponent* Inventory = ResolveMagicInventory())
		{
			BoundMagicInventory = Inventory;
			LastEvaluatedMagicInventoryRevision =
				Inventory->GetInventoryRevision();
			MagicInventoryChangedHandle = Inventory->OnInventoryChanged().AddUObject(
				this, &UMatterFluxProgressionComponent::HandleMagicInventoryChanged);
		}
	}
	if (Revision == 0)
	{
		FString Error;
		if (!ResetToStarterStateAuthority(Error))
		{
			UE_LOG(LogMatterFlux, Error,
				TEXT("Could not initialize progression after Lua load: %s"),
				*Error);
		}
		return;
	}

	// Reconcile into detached candidates. A failed Lua refresh must never leave
	// the live replicated state half-pruned or with rewards only partly applied.
	TArray<FMatterFluxItemStack> NextItems = ItemStacks.Items;
	TArray<FMatterFluxQuestState> NextQuests = QuestStates.Items;
	FName NextSelection = SelectedQuest;
	NextItems.RemoveAll(
		[&Registry](const FMatterFluxItemStack& Stack)
		{
			return !Registry->Items.Contains(Stack.ItemId);
		});
	NextQuests.RemoveAll(
		[&Registry](const FMatterFluxQuestState& State)
		{
			return !Registry->Quests.Contains(State.QuestId);
		});
	if (!NextSelection.IsNone() && !Registry->Quests.Contains(NextSelection))
	{
		NextSelection = NAME_None;
	}

	FMatterFluxQuestEvent Event;
	Event.Type = EMatterFluxQuestEventType::Refresh;
	FMatterFluxProgressionEffects Effects;
	FString Error;
	if (!FMatterFluxProgressionRules::NotifyEvent(
		*Registry, BuildEvaluationContext(), NextItems, NextQuests,
		NextSelection, Event, Effects, Error))
	{
		UE_LOG(LogMatterFlux, Error,
			TEXT("Progression reconciliation failed after Lua reload: %s"), *Error);
		return;
	}
	for (const FMatterFluxQuestRewardDefinition& Reward : Effects.Rewards)
	{
		if (Reward.Kind != EMatterFluxQuestRewardKind::Item)
		{
			continue;
		}
		int32 Previous = 0;
		int32 Current = 0;
		if (!FMatterFluxProgressionRules::AddItem(
			*Registry, NextItems, Reward.ContentId, Reward.Quantity,
			Previous, Current, Error))
		{
			UE_LOG(LogMatterFlux, Error,
				TEXT("Progression reward reconciliation failed after Lua reload: %s"),
				*Error);
			return;
		}
	}

	const bool bItemsChanged = !MatterFluxProgressionRules::SameItems(
		NextItems, ItemStacks.Items);
	const bool bQuestsChanged = !MatterFluxProgressionRules::SameQuests(
		NextQuests, QuestStates.Items);
	if (!bItemsChanged && !bQuestsChanged && NextSelection == SelectedQuest
		&& Effects.Rewards.IsEmpty())
	{
		return;
	}
	if (!ApplyEffectsAuthority(Effects, Error))
	{
		UE_LOG(LogMatterFlux, Error,
			TEXT("Progression effects failed after Lua reload: %s"), *Error);
		return;
	}

	ItemStacks.Items = MoveTemp(NextItems);
	QuestStates.Items = MoveTemp(NextQuests);
	SelectedQuest = NextSelection;
	++Revision;
	MarkAllDirty();
	ProgressionChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
}

void UMatterFluxProgressionComponent::MarkAllDirty()
{
	ItemStacks.MarkArrayDirty();
	for (FMatterFluxItemStack& Stack : ItemStacks.Items)
	{
		ItemStacks.MarkItemDirty(Stack);
	}
	QuestStates.MarkArrayDirty();
	for (FMatterFluxQuestState& State : QuestStates.Items)
	{
		QuestStates.MarkItemDirty(State);
	}
}

bool UMatterFluxProgressionComponent::ServerUseItem_Validate(
	const FName ItemId, const int32 ExpectedRevision)
{
	return !ItemId.IsNone() && ItemId.ToString().Len() <= 64
		&& ExpectedRevision >= 0 && ExpectedRevision < MAX_int32;
}

void UMatterFluxProgressionComponent::ServerUseItem_Implementation(
	const FName ItemId, const int32 ExpectedRevision)
{
	FString Error;
	if (!UseItemAuthority(ItemId, ExpectedRevision, Error))
	{
		UE_LOG(LogMatterFlux, Warning,
			TEXT("Rejected client item use for %s: %s"),
			*GetNameSafe(GetOwner()), *Error);
	}
}

bool UMatterFluxProgressionComponent::ServerSelectQuest_Validate(
	const FName QuestId)
{
	return !QuestId.IsNone() && QuestId.ToString().Len() <= 64;
}

void UMatterFluxProgressionComponent::ServerSelectQuest_Implementation(
	const FName QuestId)
{
	FString Error;
	if (!SelectQuestAuthority(QuestId, Error))
	{
		UE_LOG(LogMatterFlux, Warning,
			TEXT("Rejected client quest selection for %s: %s"),
			*GetNameSafe(GetOwner()), *Error);
	}
}
void UMatterFluxProgressionComponent::OnRep_ProgressionMetadata()
{
	// FastArray preserves element identity, not authoritative array order, when
	// an unlocked quest is inserted. Re-establish the registry-independent
	// lexical projection after the full metadata replication batch completes.
	ItemStacks.Items.Sort([](
		const FMatterFluxItemStack& A, const FMatterFluxItemStack& B)
	{
		return A.ItemId.LexicalLess(B.ItemId);
	});
	QuestStates.Items.Sort([](
		const FMatterFluxQuestState& A, const FMatterFluxQuestState& B)
	{
		return A.QuestId.LexicalLess(B.QuestId);
	});
	HandleReplicatedProgressionChanged();
}
