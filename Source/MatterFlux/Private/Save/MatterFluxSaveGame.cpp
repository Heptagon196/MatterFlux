#include "Save/MatterFluxSaveGame.h"

namespace MatterFluxSaveValidation
{
	constexpr int32 MaximumInventoryEntries = 256;
	constexpr int32 MaximumProgressionEntries = 512;
	constexpr int32 MaximumFragmentStates = 20000;
	constexpr int32 MaximumMaskCells = 1024 * 1024;
	constexpr int32 MaximumMaterialStateBytes = 1024 * 1024;
	constexpr int32 EquipmentSlotCount = 4;

	bool IsBinaryMask(const TArray<uint8>& Mask)
	{
		return Mask.Num() <= MaximumMaskCells
			&& !Mask.ContainsByPredicate(
				[](const uint8 Value) { return Value > 1; });
	}

	bool IsValidCombustionState(
		const FMatterFluxSavedCombustionState& State)
	{
		const int64 CellCount = static_cast<int64>(State.Width)
			* static_cast<int64>(State.Height);
		return !State.RuleId.IsNone()
			&& State.Width > 0
			&& State.Height > 0
			&& CellCount > 0
			&& CellCount <= MaximumMaskCells
			&& State.FuelMask.Num() == CellCount
			&& State.ResidueMask.Num() == CellCount
			&& State.BurningMask.Num() == CellCount
			&& IsBinaryMask(State.FuelMask)
			&& IsBinaryMask(State.ResidueMask)
			&& IsBinaryMask(State.BurningMask);
	}
}

void UMatterFluxSaveGame::InitializeNew(const int32 InMapSeed)
{
	SaveVersion = CurrentVersion;
	SavedAtUtc = FDateTime::UtcNow();
	MapSeed = FMath::Max(InMapSeed, 1);
	PlayerTransform = FTransform::Identity;
	MagicInventory = FMatterFluxMagicInventorySaveState();
	MagicInventory.EquippedWands.SetNum(
		MatterFluxSaveValidation::EquipmentSlotCount);
	Progression = FMatterFluxProgressionSaveState();
	WorldState = FMatterFluxWorldSaveState();
}

bool UMatterFluxSaveGame::ValidateAndMigrate(FString& OutError)
{
	using namespace MatterFluxSaveValidation;
	OutError.Reset();
	if (SaveVersion < 0 || SaveVersion > CurrentVersion)
	{
		OutError = FString::Printf(
			TEXT("unsupported save version %d"), SaveVersion);
		return false;
	}
	if (SaveVersion <= 1)
	{
		// Versions 0-1 predate item/quest persistence. Revision zero tells the
		// authoritative component to rebuild Lua-defined starter progression.
		Progression = FMatterFluxProgressionSaveState();
		Progression.Revision = 0;
		SaveVersion = CurrentVersion;
	}
	if (MapSeed <= 0 || !PlayerTransform.IsValid())
	{
		OutError = TEXT("save has an invalid map seed or player transform");
		return false;
	}
	if (MagicInventory.Spells.Num() > MaximumInventoryEntries
		|| MagicInventory.Wands.Num() > MaximumInventoryEntries
		|| MagicInventory.EquippedWands.Num() != EquipmentSlotCount
		|| MagicInventory.InventoryRevision < 0
		|| MagicInventory.ActiveEquipmentSlot < 0
		|| MagicInventory.ActiveEquipmentSlot >= EquipmentSlotCount)
	{
		OutError = TEXT("save has invalid magic inventory metadata");
		return false;
	}
	TSet<FName> SeenSpellIds;
	for (const FMatterFluxSavedSpell& Spell : MagicInventory.Spells)
	{
		if (Spell.SpellId.IsNone()
			|| Spell.Quantity <= 0
			|| SeenSpellIds.Contains(Spell.SpellId))
		{
			OutError = TEXT("save contains an invalid spell stack");
			return false;
		}
		SeenSpellIds.Add(Spell.SpellId);
	}
	TSet<FGuid> SeenWandIds;
	for (const FMatterFluxSavedWand& Wand : MagicInventory.Wands)
	{
		if (!Wand.InstanceId.IsValid()
			|| Wand.DefinitionId.IsNone()
			|| SeenWandIds.Contains(Wand.InstanceId)
			|| Wand.SpellSlots.Num() > MaximumInventoryEntries
			|| !FMath::IsFinite(Wand.Mana)
			|| Wand.Mana < 0.0f
			|| Wand.DeckCursor < 0
			|| Wand.CastSerial < 0
			|| !FMath::IsFinite(Wand.CastCooldownRemaining)
			|| Wand.CastCooldownRemaining < 0.0f
			|| Wand.CastCooldownRemaining > 3600.0f)
		{
			OutError = TEXT("save contains an invalid wand");
			return false;
		}
		SeenWandIds.Add(Wand.InstanceId);
	}
	TSet<FGuid> SeenEquippedWands;
	for (const FGuid EquippedWand : MagicInventory.EquippedWands)
	{
		if (EquippedWand.IsValid()
			&& (!SeenWandIds.Contains(EquippedWand)
				|| SeenEquippedWands.Contains(EquippedWand)))
		{
			OutError = TEXT("save contains an invalid equipped wand reference");
			return false;
		}
		if (EquippedWand.IsValid())
		{
			SeenEquippedWands.Add(EquippedWand);
		}
	}
	if (Progression.Items.Num() > MaximumProgressionEntries
		|| Progression.Quests.Num() > MaximumProgressionEntries
		|| Progression.Revision < 0
		|| (Progression.Revision == 0
			&& (!Progression.Items.IsEmpty() || !Progression.Quests.IsEmpty()
				|| !Progression.SelectedQuest.IsNone())))
	{
		OutError = TEXT("save has invalid progression metadata");
		return false;
	}
	TSet<FName> SeenItemIds;
	for (const FMatterFluxSavedItemStack& Item : Progression.Items)
	{
		if (Item.ItemId.IsNone() || Item.Quantity <= 0
			|| SeenItemIds.Contains(Item.ItemId))
		{
			OutError = TEXT("save contains an invalid item stack");
			return false;
		}
		SeenItemIds.Add(Item.ItemId);
	}
	TSet<FName> SeenQuestIds;
	for (const FMatterFluxSavedQuestState& Quest : Progression.Quests)
	{
		if (Quest.QuestId.IsNone() || Quest.Status > 3 || Quest.Progress < 0
			|| SeenQuestIds.Contains(Quest.QuestId))
		{
			OutError = TEXT("save contains an invalid quest state");
			return false;
		}
		SeenQuestIds.Add(Quest.QuestId);
	}
	if (!Progression.SelectedQuest.IsNone()
		&& !SeenQuestIds.Contains(Progression.SelectedQuest))
	{
		OutError = TEXT("save selects a quest that is not present");
		return false;
	}
	if (WorldState.MaterialActiveState.Num() > MaximumMaterialStateBytes
		|| WorldState.FragmentSources.Num() > MaximumFragmentStates
		|| WorldState.RemovedFragmentSourceIds.Num() > MaximumFragmentStates
		|| !FMath::IsFinite(WorldState.GroundCombustionAccumulator)
		|| WorldState.GroundCombustionAccumulator < 0.0f
		|| WorldState.GroundCombustionRevision < 0)
	{
		OutError = TEXT("save exceeds the supported world-state budget");
		return false;
	}
	TSet<FGuid> SeenSourceIds;
	for (const FMatterFluxSavedFragmentSourceState& State
		: WorldState.FragmentSources)
	{
		if (!State.SourceId.IsValid()
			|| SeenSourceIds.Contains(State.SourceId)
			|| State.Revision < 0
			|| !IsBinaryMask(State.RuntimeMask)
			|| !FMath::IsFinite(State.CombustionAccumulator)
			|| State.CombustionAccumulator < 0.0f
			|| State.TotalSmokeEmissionCount < 0
			|| !State.ActorTransform.IsValid()
			|| (State.bHasCombustionState
				&& !IsValidCombustionState(State.CombustionState)))
		{
			OutError = TEXT("save contains an invalid fragment-source state");
			return false;
		}
		SeenSourceIds.Add(State.SourceId);
	}
	TSet<FGuid> SeenRemovedSourceIds;
	for (const FGuid RemovedSourceId
		: WorldState.RemovedFragmentSourceIds)
	{
		if (!RemovedSourceId.IsValid()
			|| SeenRemovedSourceIds.Contains(RemovedSourceId)
			|| SeenSourceIds.Contains(RemovedSourceId))
		{
			OutError = TEXT("save contains an invalid removed-source reference");
			return false;
		}
		SeenRemovedSourceIds.Add(RemovedSourceId);
	}
	if (WorldState.bHasGroundCombustionState
		&& !IsValidCombustionState(WorldState.GroundCombustionState))
	{
		OutError = TEXT("save contains an invalid ground-combustion state");
		return false;
	}
	TSet<FGuid> SeenIgnitionSourceIds;
	for (const FGuid SourceId : WorldState.SourcesThatIgnitedGround)
	{
		if (!SourceId.IsValid()
			|| SeenIgnitionSourceIds.Contains(SourceId))
		{
			OutError = TEXT("save contains an invalid ignition-source reference");
			return false;
		}
		SeenIgnitionSourceIds.Add(SourceId);
	}
	SaveVersion = CurrentVersion;
	return true;
}

void UMatterFluxSaveMetadata::Initialize()
{
	MetadataVersion = CurrentVersion;
	Slots.Reset();
}

bool UMatterFluxSaveMetadata::ValidateAndRepair()
{
	bool bWasValid = MetadataVersion == CurrentVersion;
	MetadataVersion = CurrentVersion;

	TSet<int32> SeenSlotIndices;
	TArray<FMatterFluxSaveSlotInfo> RepairedSlots;
	RepairedSlots.Reserve(Slots.Num());
	for (const FMatterFluxSaveSlotInfo& Slot : Slots)
	{
		const FString NormalizedName = NormalizeDisplayName(Slot.DisplayName);
		const bool bOccupiedSlotValid = Slot.bOccupied
			&& Slot.SlotIndex >= 0
			&& Slot.MapSeed > 0
			&& Slot.SavedAtUtc.GetTicks() > 0
			&& !SeenSlotIndices.Contains(Slot.SlotIndex);
		if (bOccupiedSlotValid)
		{
			SeenSlotIndices.Add(Slot.SlotIndex);
			FMatterFluxSaveSlotInfo& Repaired = RepairedSlots.Add_GetRef(Slot);
			Repaired.DisplayName = NormalizedName;
			bWasValid = bWasValid && NormalizedName == Slot.DisplayName;
		}
		else
		{
			bWasValid = false;
		}
	}
	RepairedSlots.Sort([](
		const FMatterFluxSaveSlotInfo& Left,
		const FMatterFluxSaveSlotInfo& Right)
	{
		return Left.SlotIndex < Right.SlotIndex;
	});
	if (bWasValid)
	{
		for (int32 Index = 0; Index < Slots.Num(); ++Index)
		{
			if (Slots[Index].SlotIndex != RepairedSlots[Index].SlotIndex)
			{
				bWasValid = false;
				break;
			}
		}
	}
	Slots = MoveTemp(RepairedSlots);
	return bWasValid;
}

int32 UMatterFluxSaveMetadata::GetNextAvailableSlotIndex() const
{
	TSet<int32> UsedSlotIndices;
	for (const FMatterFluxSaveSlotInfo& Slot : Slots)
	{
		if (Slot.bOccupied && Slot.SlotIndex >= 0)
		{
			UsedSlotIndices.Add(Slot.SlotIndex);
		}
	}
	for (int32 Candidate = 0; Candidate < MAX_int32; ++Candidate)
	{
		if (!UsedSlotIndices.Contains(Candidate))
		{
			return Candidate;
		}
	}
	return INDEX_NONE;
}

FString UMatterFluxSaveMetadata::NormalizeDisplayName(
	const FString& InDisplayName)
{
	FString Result = InDisplayName.TrimStartAndEnd();
	Result.ReplaceInline(TEXT("\r"), TEXT(" "));
	Result.ReplaceInline(TEXT("\n"), TEXT(" "));
	Result.ReplaceInline(TEXT("\t"), TEXT(" "));
	if (Result.Len() > 32)
	{
		Result.LeftInline(32, EAllowShrinking::No);
	}
	return Result;
}
